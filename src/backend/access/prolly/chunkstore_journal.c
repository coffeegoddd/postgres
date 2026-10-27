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
 * naming it, both under the database's append lock, so every append after goes to it: a
 * backend reads the segments again, if a manifest was written since, before
 * it appends.  Every segment named is read; a backend notices a manifest
 * that drops one (a collection's) at its next lookup, and forgets the
 * segment's records.
 *
 * Appending.  A backend gathers its transaction's chunks and appends them
 * in batches, and the rest when it commits.  An append reserves its range
 * at the segment's end and WAL-logs its bytes under the database's append lock, and
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
 * under what its chunks name; a settled one with the generations its
 * chunks were born in, the collections that settled them, and how many of
 * them are thought garbage), the segments, the files and segments
 * retiring (no longer named, kept until whoever may read them has ended),
 * the last id given, the root as of its writing, the GC generation and
 * when each generation the settled files hold ended; it
 * is replaced whole (written aside, then renamed over the old one).  Every
 * byte of a table file is WAL-logged before it is written, as journal
 * appends are: bytes new to the store as they are, those copied from its
 * own files (a collection's) as where they come from (a COPY record, which
 * redo copies again from its own files: "Copies in redo", below); the
 * file is synced before the manifest naming it is logged; a manifest's
 * record carries its contents, and an unlink's names the file, which isn't
 * removed while an online backup runs.  One writer of table files (and
 * sealer of segments) at a time (a lock on the manifest, until its
 * transaction ends); a file or segment
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
 * not.  The marked chunks are copied as stored, each checked against its
 * hash, to a new settled
 * table file before the window; the window takes the database's append lock, so no
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
#ifndef WIN32
#include <sys/mman.h>
#endif
#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/prolly_autovacuum.h"
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
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "journal_index.h"
#include "lib/ilist.h"
#include "commands/vacuum.h"
#include "catalog/pg_tablespace_d.h"
#include "common/hashfn.h"
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
#include "storage/sync.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define JOURNAL_FILE		"prolly_journal"	/* segment 0's */
#define SEGMENT_PREFIX		"prolly_journal_"	/* the others', id in hex */
#define MANIFEST_FILE		"prolly_manifest"
#define MANIFEST_TEMP		"prolly_manifest.tmp"

/*
 * A clone's files, received whole: table files whose ids have this bit
 * (from the key the remote names the file by), named apart from the
 * store's own (RECEIVED_PREFIX), so nothing that tidies unnamed files
 * removes them before they are adopted or forgotten.
 */
#define RECEIVED_ID			(UINT64CONST(1) << 63)
#define RECEIVED_PREFIX		"prolly_received_"
#define MANIFEST_MAGIC		"PRLYMAN1"
#define MANIFEST_VERSION	6
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

/*
 * Bytes copied from the store's own files: where they come from and their
 * CRC, not the bytes, which redo copies again from its own files (#564)
 */
#define XLOG_CHUNKS_COPY		0x80

typedef struct xl_chunks_copy
{
	xl_prolly_header hdr;
	Oid			dboid;
	Oid			spcoid;
	uint64		id;				/* the table file (or segment) written */
	uint64		offset;			/* where in it */
	Oid			src_dboid;		/* the sources' database: this one, or
								 * CREATE DATABASE's template */
	Oid			src_spcoid;
	bool		segment;		/* id is a segment's */
	uint32		len;			/* the bytes the sources make */
	pg_crc32c	crc;			/* their CRC-32C */
	uint32		nsources;
	/* the sources follow, COPY_SOURCE_SIZE bytes each */
} xl_chunks_copy;

#define SizeOfChunksCopy	(offsetof(xl_chunks_copy, nsources) + sizeof(uint32))

/* a copy's source: bytes as they are, or a journal record's chunk */
#define COPY_TABLE_BYTES	1	/* a table file's */
#define COPY_SEGMENT_BYTES	2	/* a segment's */
#define COPY_SEGMENT_CHUNK	3	/* a segment's CHUNK or XCHUNK record (offset,
								 * len), as a table file holds its chunk */
#define COPY_SOURCE_SIZE	21	/* kind, id, offset, len */

typedef struct CopySource
{
	uint8		kind;			/* 0: none, the bytes are new */
	uint64		id;
	uint64		offset;
	uint32		len;
} CopySource;

/* the sources of bytes about to be written */
typedef struct CopyRun
{
	Oid			src_dboid;
	Oid			src_spcoid;
	int			n;
	int			max;
	CopySource *sources;
} CopyRun;

/*
 * Where the chunk a writer's next() gave last is in the store's files,
 * set by next() (kind 0: nowhere, or it doesn't say)
 */
typedef struct CopyFrom
{
	CopySource	src;
	const char *bytes;			/* COPY_TABLE_BYTES: the src.len bytes there */
} CopyFrom;

/* a table file's and a segment's entries in the manifest, in bytes */
#define MANIFEST_FILE_ENTRY		33	/* id, chunks, size, flags, epochs,
										 * garbage */
#define MANIFEST_EPOCH_ENTRY	12	/* epoch, ended at */
#define MANIFEST_SEGMENT_ENTRY	16	/* id, sealed at */

/* a table file's flags in the manifest */
#define MANIFEST_FILE_SETTLED	0x01	/* closed under what its chunks name */
#define MANIFEST_FILE_REPAIRED	0x02	/* settled, of history a repair put back
										 * in older generations (or of such a
										 * file rewritten): its chunks may name
										 * newer ones *after all*, and a retire
										 * reads them all, not its section */

/* the manifest: the table files, the journal's segments, and the ids given */
typedef struct ManifestFile
{
	uint64		id;
	uint32		nchunks;
	uint64		size;
	uint8		flags;
	uint32		epoch_lo;		/* a settled file's generations: its chunks
								 * were born in collections epoch_lo to
								 * epoch_hi (0: not settled, none) */
	uint32		epoch_hi;
	uint32		garbage;		/* its chunks thought garbage, at least */
} ManifestFile;

/* a collection that settled chunks: its generation, and when it ended */
typedef struct ManifestEpoch
{
	uint32		epoch;
	TimestampTz ended;
} ManifestEpoch;

typedef struct Manifest
{
	uint64		last_id;		/* table file and segment ids given so far */
	uint32		gc_generation;
	ProllyHash	root;			/* as of the manifest's writing: the root
								 * while no segment it names has a ROOT */
	ProllyHash	base;			/* the root before the oldest ROOT the
								 * segments it names hold, as a collection
								 * left it (none: not known) */
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
	int			nepochs;
	ManifestEpoch *epochs;		/* oldest first: the generations the settled
								 * files hold, as far back as the oldest */
} Manifest;

/* a table file a backend reads */
typedef struct TableFile
{
	uint64		id;
	char	   *path;
	File		file;
	uint64		size;
	bool		settled;
	uint32		epoch_lo;		/* its first generation (0: not settled) */
	ProllyTableFile *tf;
	void	   *mapped;			/* its index, mapped (NULL: read) */
	Size		mapped_len;
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

/*
 * A transaction that told keep() of a chunk (xid invalid: none), or one
 * begun on a hot standby that did, which has no xid: held is its local
 * transaction id (invalid: none).
 */
typedef struct KeepTouch
{
	Oid			dboid;
	Oid			spcoid;
	TransactionId xid;
	LocalTransactionId held;
} KeepTouch;

/* shared by every backend */
typedef struct JournalShared
{
	pg_atomic_uint64 manifests;	/* manifests written or replayed */

	/* appends reserved and not written yet; under mutex */
	slock_t		mutex;
	ConditionVariable written;	/* an append's bytes were written */
	CheckedEnd	checked[CHECKED_ENDS];	/* under mutex */
	int			next_checked;
	int			inflight;		/* ranges of len > 0 */
	TimestampTz last_at;		/* the last append's time; under mutex */

	/* under mutex */
	KeepSet		keeps[KEEP_COLLECTIONS];
	pg_atomic_uint64 keeps_begun;	/* keep sets begun */

	/*
	 * The store whose collection's swap is being replayed (dboid invalid:
	 * none), under mutex, and broadcast once it has been.
	 */
	Oid			swap_dboid;
	Oid			swap_spcoid;
	ConditionVariable swapped;

	/*
	 * A sync request the checkpointer's queue refused: the next checkpoint
	 * fsyncs every store's files instead.
	 */
	pg_atomic_uint32 sync_all;

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
	uint32		raw_len;		/* (pending) the chunk's own length */
} IndexEntry;

/*
 * A journal segment a backend reads, and (the last) appends to: its file.
 * How far it has been read, and what was skipped, the shared index keeps
 * (JournalSegScan); scanned is a collection's own reading position.
 */
typedef struct Segment
{
	uint64		id;
	char	   *path;
	File		file;
	uint64		scanned;
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
	TimestampTz at;				/* when reserved: a ROOT record's time */
	bool		sync_refused;	/* sync_refused() as it was reserved */
} Append;

struct ProllyChunkStore
{
	Oid			dboid;
	Oid			spcoid;

	List	   *segments;		/* Segment *, as the manifest last read names */
	List	   *other_segments; /* Segment *, others a newer manifest names */
	JournalIndex *ix;			/* the chunks in the segments, shared */
	ProllyHash	root;			/* the root as last read (the index's) */
	uint64		root_segment;	/* where the last ROOT record seen is; 0, 0: */
	uint64		root_offset;	/* the manifest's root, or none */

	List	   *tables;			/* TableFile *, as the manifest last read named */
	List	   *received;		/* TableFile *, a clone's files, whole */
	bool		received_open;	/* received is those on disk */
	uint64		manifests_seen; /* the shared count when it was read */
	uint64		segments_seen;	/* likewise, for the segments */
	uint32		gc_generation;	/* the manifest's, as last read */
	ProllyHash	base;			/* likewise: the root before its segments'
								 * oldest ROOT, if known */

	/* the GC generation the transaction (lxid) began in */
	LocalTransactionId xact_lxid;
	uint32		xact_generation;
	uint64		xact_flushes;	/* flushes as it began */
	ProllyChunkGc *gc;			/* a collection the transaction runs */

	/*
	 * Chunks a walk said it reads next (prefetch()), read together ahead of
	 * it: each given out once, all forgotten as the transaction ends.
	 */
	MemoryContext ahead_cxt;
	HTAB	   *ahead;
	Size		ahead_bytes;

	/* chunks the store hasn't, read from elsewhere (read_through()) */
	ProllyChunkGet through;
	ProllyChunkHint through_hint;
	void	   *through_arg;

	/* this transaction's */
	StringInfoData pending;		/* records not appended yet */
	HTAB	   *pending_index;
	HTAB	   *transient;		/* during recovery: chunks a read made, by
								 * hash, until its transaction ends */
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
static void defer_unlink(Oid dboid, Oid spcoid, bool segment, uint64 id);
typedef void (*DatabaseDirFn) (const char *dir, Oid dboid, Oid spcoid);
static void each_database_dir(DatabaseDirFn fn);
static void retire_files(Oid dboid, Oid spcoid, const Manifest *before,
						 const Manifest *after);
static void keep_held(ProllyChunkStore *s, JournalShared *js, KeepTouch *t);
static void standby_readers_note(Oid dboid);
static void standby_readers_resolve(Oid dboid);
static void check_dropped(ProllyChunkStore *s, const ProllyHash *root);
static void gc_abort(ProllyChunkGc *gc);
static void gc_abort_sub(ProllyChunkGc *gc, int level);
static XLogRecPtr logical_horizon(Oid dboid);
static int	append_ranges(void);
static uint32 gc_record(Segment *g, uint64 offset, uint64 size, char **rec,
						Size *reclen);

/* ----------------------------------------------------------------
 * Records
 * ----------------------------------------------------------------
 */

/* A table by chunk hash (a ProllyHash, hash_key()'s). */
static uint32
chunk_hash_key_hash(const void *key, Size keysize)
{
	return prolly_hash_bucket(((const ProllyHash *) key)->bytes);
}

static void
hash_key(ProllyHash *key, const ProllyHash *hash)
{
	memset(key, 0, sizeof(ProllyHash));
	key->len = hash->len;
	memcpy(key->bytes, hash->bytes, hash->len);
}

/*
 * Decoded chunks the session's store read from its files, kept by hash in
 * the node cache (prolly_nodecache.h) for every backend's later reads.  A
 * chunk is named by its content, so one kept is the chunk whatever happens
 * to the store after; but not that the store still has it: a collection
 * may remove it.  So what is kept is of the store as of a collection's
 * generation (the manifest's gc_generation), and a read finds only what
 * was kept in the generation it reads in: one after a collection finds
 * nothing of what was removed, as the store's own reads then do.  Offered:
 * those read from the files (checked as they are read), which the cache
 * keeps once read again lately, so a read of many chunks once each, as a
 * scan or a fold of many rows reads leaves, neither churns it nor pays to
 * fill it; and the chunks a transaction appends as it commits
 * (pending_cached()), kept at once, once it has.  Not while pending or
 * transient, which an abort takes back, nor one read through a source.
 */
static const ProllyNodeCacheRoutine *
node_cache(void)
{
	static const ProllyNodeCacheRoutine *nc = NULL;
	static bool looked = false;

	if (!looked)
	{
		nc = prolly_node_cache();
		/* (one off, its size 0, is none: no lookup, nor a miss counted) */
		if (nc != NULL &&
			(!PROLLY_HAS(nc, lookup) || !PROLLY_HAS(nc, insert) ||
			 !PROLLY_HAS(nc, capacity) || nc->capacity() == 0))
			nc = NULL;
		looked = true;
	}
	return nc;
}

static void
cache_scope(const ProllyChunkStore *s, uint32 generation,
			ProllyNodeCacheScope *scope)
{
	scope->dboid = s->dboid;
	scope->spcoid = s->spcoid;
	scope->generation = generation;
}

static bool
chunk_cache_take(const ProllyChunkStore *s, const ProllyHash *key,
				 StringInfo out, uint32 generation)
{
	const ProllyNodeCacheRoutine *nc = node_cache();
	ProllyNodeCacheScope scope;

	if (nc == NULL)
		return false;
	cache_scope(s, generation, &scope);
	if (nc->lookup(&scope, key, out))
	{
		pgstat_count_prolly(PGSTAT_PROLLY_NODE_CACHE_HITS, 1);
		return true;
	}
	pgstat_count_prolly(PGSTAT_PROLLY_NODE_CACHE_MISSES, 1);
	return false;
}

static void
chunk_cache_put(const ProllyChunkStore *s, const ProllyHash *key,
				const char *data, uint32 len, uint32 generation, bool at_once)
{
	const ProllyNodeCacheRoutine *nc = node_cache();
	ProllyNodeCacheScope scope;

	if (nc == NULL)
		return;
	cache_scope(s, generation, &scope);
	nc->insert(&scope, key, data, len, at_once);
}

static Size
chunk_cache_limit(void)
{
	const ProllyNodeCacheRoutine *nc = node_cache();

	return nc != NULL ? nc->capacity() : 0;
}

/*
 * Chunks a transaction that writes knows are stored without asking the
 * index: those a stored chunk it read names.  The store never holds a
 * chunk without what it names, and only a collection's swap drops one, so
 * while none happened since the transaction began (xact_straddles()),
 * what a chunk it read from the store's files names is the store's still,
 * and a pending chunk naming it needs no index probe for it
 * (check_reference()).  A chunk from the cache counts too: the cache holds
 * only chunks of the store's GC generation now, which must be the one the
 * transaction began in: none was dropped since.
 * Only chunks read as the transaction has pending ones are taken in (a
 * read-only transaction pays nothing), at most TRUSTED_MAX_CHUNKS (a
 * reference past them is probed for, as without the set); the set goes
 * with the transaction.
 */
#define TRUSTED_MAX_CHUNKS	(256 * 1024)	/* about 24 MB */

static bool xact_straddles(ProllyChunkStore *s);

static HTAB *trusted = NULL;	/* in TopTransactionContext */
static LocalTransactionId trusted_lxid = InvalidLocalTransactionId;

static void
trust_ref(const ProllyHash *ref, void *arg)
{
	ProllyHash	key;

	if (hash_get_num_entries(trusted) >= TRUSTED_MAX_CHUNKS)
		return;
	hash_key(&key, ref);
	(void) hash_search(trusted, &key, HASH_ENTER, NULL);
}

/* What chunk (read stored, in GC generation generation) names, trusted. */
static void
trust_named(ProllyChunkStore *s, const char *chunk, Size len,
			uint32 generation)
{
	if (s != session_store || hash_get_num_entries(s->pending_index) == 0 ||
		MyProc->vxid.lxid != s->xact_lxid ||
		generation != s->xact_generation || xact_straddles(s))
		return;
	if (trusted == NULL || trusted_lxid != MyProc->vxid.lxid)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hash = chunk_hash_key_hash;
		ctl.hcxt = TopTransactionContext;
		trusted = hash_create("prolly trusted chunks", 1024, &ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
		trusted_lxid = MyProc->vxid.lxid;
	}
	prolly_chunk_references(chunk, len, trust_ref, NULL);
}

static bool
trusted_has(ProllyChunkStore *s, const ProllyHash *key)
{
	return trusted != NULL && trusted_lxid == MyProc->vxid.lxid &&
		!xact_straddles(s) &&
		hash_search(trusted, key, HASH_FIND, NULL) != NULL;
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

/* what a sync request names, in its tag's relNumber */
#define SYNC_SEGMENT		0
#define SYNC_TABLE_FILE		1

static JournalShared *shared(void);

/*
 * Whether sync requests are to be taken as refused (by the queue), for a
 * test: asked outside a critical section, as it may allocate.
 */
static bool
sync_refused(void)
{
	return IS_INJECTION_POINT_ATTACHED("prolly-sync-request-refused");
}

/*
 * Ask the next checkpoint to fsync a segment (or a table file) just
 * written: after the write, as md.c asks for a relation's, so a checkpoint
 * that takes the request fsyncs the bytes (one taking it before them
 * would fsync the file without them, and no later checkpoint would).
 * Before the writer lets checkpoints go on (its append's range let go, or
 * DELAY_CHKPT_START cleared), so a checkpoint whose redo point follows the
 * write's WAL record, which waits for the writer, has the request.  Never
 * waits nor allocates (it may run in a critical section): a request the
 * queue refuses (or refused, sync_refused()'s) makes the next checkpoint
 * fsync everything instead.
 */
static void
request_sync(Oid dboid, Oid spcoid, int kind, uint64 id, bool refused)
{
	FileTag		tag;

	memset(&tag, 0, sizeof(tag));
	tag.handler = SYNC_HANDLER_PROLLY_CHUNKS;
	tag.forknum = (int16) prolly_chunkstore_journal.info.impl_id;
	tag.rlocator.spcOid = spcoid;
	tag.rlocator.dbOid = dboid;
	tag.rlocator.relNumber = kind;
	tag.segno = id;
	if (refused || !RegisterSyncRequest(&tag, SYNC_REQUEST, false))
		pg_atomic_write_u32(&shared()->sync_all, 1);
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
	bool		refused;

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = dboid;
	xlrec.spcoid = spcoid;
	xlrec.segment = segment;
	xlrec.offset = offset;

	refused = sync_refused();
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
	request_sync(dboid, spcoid, SYNC_SEGMENT, segment, refused);
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
 * caller holds the database's append lock.
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

static void scan_segment(ProllyChunkStore *s, Segment *g);

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
 * reserved there meanwhile: the caller holds the database's append lock, or those
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
 *
 * A transaction of a hot standby can't be given an xid, and the
 * collections that drop chunks run on the primary, which knows nothing of
 * it.  It is noted as holding pointers instead (keep_held()), so that the
 * replay of a swap cancels it before the store stops naming what it holds.
 * One begun on a standby promoted since stays noted so: a collection on
 * the new primary waits for it, and keeps what it is told of now.
 */
static void
journal_keep(ProllyChunkStore *s, const ProllyHash *hash)
{
	JournalShared *js = shared();
	KeepTouch  *t = &touches(js)[MyProcNumber];
	bool		recovery = RecoveryInProgress();
	bool		standby = recovery || TransactionStartedDuringRecovery();
	TransactionId xid = standby ? InvalidTransactionId : GetTopTransactionId();
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
		ctl.hash = chunk_hash_key_hash;
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = TopTransactionContext;
		kept_here = hash_create("prolly chunks kept", 64, &ctl,
								HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
		kept_here_begun = begun;
	}
	(void) hash_search(kept_here, &key, HASH_FIND, &found);
	if (found)
		return;

	if (standby)
	{
		keep_held(s, js, t);

		/*
		 * (read again once noted: a collection begins only after recovery
		 * has ended, so either it sees this transaction noted, or this
		 * sees recovery over and adds the chunk to its keep set)
		 */
		recovery = RecoveryInProgress();
	}
	if (!recovery)
	{
		jindex_append_lock(s->ix, false);
		SpinLockAcquire(&js->mutex);
		if (TransactionIdIsValid(xid) && !TransactionIdIsValid(t->xid))
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
		jindex_append_unlock(s->ix);
	}
	if (!journal_has(s, hash))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to a concurrent collection of the chunk store"),
				 errdetail("A value stored by content was collected meanwhile."),
				 errhint("The transaction might succeed if retried.")));
	(void) hash_search(kept_here, &key, HASH_ENTER, NULL);
}

/*
 * This transaction of a hot standby (or begun as one) holds pointers: its
 * local id noted, for a swap's replay to resolve a recovery conflict with
 * before the store stops naming what it may read, and for a collection
 * after a promotion to wait for.  Not while a swap of the store is being
 * replayed: the conflict is being resolved with the holders noted before,
 * so a transaction not among them waits for the swap and then finds
 * whether its chunk survived.  One among them doesn't wait (the replay
 * waits for it): it is cancelled, or ends, before the swap stops naming
 * what it keeps now.  Never in a parallel worker: its local id isn't its
 * leader's, so it would wait for a replay that waits for the leader.
 */
static void
keep_held(ProllyChunkStore *s, JournalShared *js, KeepTouch *t)
{
	Assert(!IsParallelWorker());

	for (;;)
	{
		bool		replaying;

		SpinLockAcquire(&js->mutex);
		replaying = js->swap_dboid == s->dboid &&
			js->swap_spcoid == s->spcoid && t->held != MyProc->vxid.lxid;
		if (!replaying && t->held != MyProc->vxid.lxid)
		{
			t->dboid = s->dboid;
			t->spcoid = s->spcoid;
			t->held = MyProc->vxid.lxid;
		}
		SpinLockRelease(&js->mutex);
		if (!replaying)
			break;
		ConditionVariableSleep(&js->swapped, WAIT_EVENT_PROLLY_SWAP_REPLAY);
	}
	ConditionVariableCancelSleep();
}

/*
 * A swap of a store is being replayed: the standby's transactions noted as
 * holding its pointers are cancelled, after max_standby_streaming_delay,
 * before the new manifest stops naming what they hold.  A transaction
 * that tells keep() of a chunk meanwhile waits for swap_replayed().
 */
static void
swap_holders_resolve(Oid dboid, Oid spcoid)
{
	JournalShared *js = shared();
	KeepTouch  *ts = touches(js);
	VirtualTransactionId *holders =
		palloc(sizeof(VirtualTransactionId) * (js->nranges + 1));
	int			n = 0;

	SpinLockAcquire(&js->mutex);
	js->swap_dboid = dboid;
	js->swap_spcoid = spcoid;
	for (int i = 0; i < js->nranges; i++)
	{
		KeepTouch  *t = &ts[i];

		if (t->held != InvalidLocalTransactionId && t->dboid == dboid &&
			t->spcoid == spcoid)
		{
			holders[n].procNumber = i;
			holders[n].localTransactionId = t->held;
			n++;
		}
	}
	SpinLockRelease(&js->mutex);
	SetInvalidVirtualTransactionId(holders[n]);
	ResolveRecoveryConflictWithFileReaders(holders);
	pfree(holders);
}

/* The swap has been replayed: keep() is told of chunks again. */
static void
swap_replayed(void)
{
	JournalShared *js = shared();

	SpinLockAcquire(&js->mutex);
	js->swap_dboid = InvalidOid;
	js->swap_spcoid = InvalidOid;
	SpinLockRelease(&js->mutex);
	ConditionVariableBroadcast(&js->swapped);
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
	if (!TransactionIdIsValid(t->xid) && t->held == InvalidLocalTransactionId)
		return;
	SpinLockAcquire(&js->mutex);
	t->xid = InvalidTransactionId;
	t->held = InvalidLocalTransactionId;
	SpinLockRelease(&js->mutex);
}

/*
 * A collection begins keeping what keep() is told of, and will wait for
 * the transactions told of one before: its keep set, or -1 if none is free
 * (it then keeps every chunk).  Those with an xid are added to *xids; those
 * begun on this server as a hot standby, before a promotion, to *held
 * (*nheld of them).
 */
static int
keep_begin(ProllyChunkStore *s, List **xids, VirtualTransactionId **held,
		   int *nheld)
{
	JournalShared *js = shared();
	KeepTouch  *ts = touches(js);
	TransactionId *found = palloc(sizeof(TransactionId) * js->nranges);
	int			nfound = 0;
	int			slot = -1;

	*held = palloc(sizeof(VirtualTransactionId) * js->nranges);
	*nheld = 0;

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

		if (t->dboid != s->dboid || t->spcoid != s->spcoid)
			continue;
		if (TransactionIdIsValid(t->xid))
			found[nfound++] = t->xid;
		if (t->held != InvalidLocalTransactionId)
		{
			(*held)[*nheld].procNumber = i;
			(*held)[*nheld].localTransactionId = t->held;
			(*nheld)++;
		}
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
	bool		checked = false;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < CHECKED_ENDS && !checked; i++)
	{
		CheckedEnd *c = &js->checked[i];

		checked = c->segment == g->id && c->dboid == s->dboid &&
			c->spcoid == s->spcoid;
	}
	SpinLockRelease(&js->mutex);
	return checked;
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
 * The segment's end, where the next append goes, under the database's append lock.
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
	jindex_lock(s->ix, true);
	scan_segment(s, g);
	end = Max(jindex_segment_of(s->ix, g->id)->scanned, in_flight_end(s, g->id));
	jindex_unlock(s->ix);
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
	SpinLockAcquire(&js->mutex);
	js->checked[js->next_checked].dboid = s->dboid;
	js->checked[js->next_checked].spcoid = s->spcoid;
	js->checked[js->next_checked].segment = g->id;
	js->next_checked = (js->next_checked + 1) % CHECKED_ENDS;
	SpinLockRelease(&js->mutex);
	return size;
}

/*
 * Reserve len bytes at the end of the segment appends go to, and WAL-log
 * data for them, under the database's append lock; the bytes are written by
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
	{
		jindex_lock(s->ix, true);
		scan_segment(s, g);
		jindex_unlock(s->ix);
	}
	jindex_append_lock(s->ix, true);
	refresh_segments(s);
	if (checked != NULL && s->gc_generation != *checked)
	{
		jindex_append_unlock(s->ix);
		return false;
	}
	g = current_segment(s);
	size = segment_end(s, g);
	if (FileTruncate(g->file, size + len, WAIT_EVENT_DATA_FILE_EXTEND) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend file \"%s\": %m", g->path)));

	/*
	 * (taken under the database's append lock, so a later place in its
	 * journal has no earlier time: a reflog is newest first by place; and
	 * never before the last append's, any database's, under the mutex, so
	 * not across a clock stepped back either)
	 */
	{
		TimestampTz now = GetCurrentTimestamp();

		SpinLockAcquire(&js->mutex);
		a->at = Max(now, js->last_at);
		js->last_at = a->at;
		SpinLockRelease(&js->mutex);
	}

	a->sync_refused = sync_refused();
	INJECTION_POINT("prolly-append-reserving", NULL);
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
	jindex_append_unlock(s->ix);

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
	request_sync(r->dboid, r->spcoid, SYNC_SEGMENT, r->segment, a->sync_refused);
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

/*
 * Is a ROOT record at (segment, offset) later than the root's, as the
 * index's readers have it?
 */
static bool
root_later(JournalScan *js, uint64 segment, uint64 offset)
{
	return segment > js->root_segment ||
		(segment == js->root_segment && offset >= js->root_offset);
}

static void
set_root(JournalScan *js, const ProllyHash *h, uint64 segment, uint64 offset)
{
	js->root = *h;
	js->root_segment = segment;
	js->root_offset = offset;
}

/*
 * The local segment of id: one the manifest this backend last read names,
 * or else one another backend's newer manifest does, opened as it is met.
 */
static Segment *
local_segment(ProllyChunkStore *s, uint64 id)
{
	Segment    *g = segment_of(s, id);
	ListCell   *lc;
	MemoryContext old;

	if (g != NULL)
		return g;
	foreach(lc, s->other_segments)
	{
		if (((Segment *) lfirst(lc))->id == id)
			return lfirst(lc);
	}
	old = MemoryContextSwitchTo(TopMemoryContext);
	g = palloc0(sizeof(Segment));
	g->id = id;
	g->path = segment_path(s->dboid, s->spcoid, id);
	g->file = -1;
	s->other_segments = lappend(s->other_segments, g);
	MemoryContextSwitchTo(old);
	return g;
}

static uint64 index_range(ProllyChunkStore *s, Segment *g, JournalSegScan *sg,
						  uint64 offset, uint64 end, char **rec, Size *reclen);

/*
 * Has the transaction of the PAD at offset pad of g ended with the PAD
 * still a PAD?  Then it never becomes a ROOT (the transaction aborted):
 * a committed one's ROOT is written before its transaction is seen to
 * end (on a standby, before its xid leaves the known-assigned ones).
 */
static bool
pad_ended(Segment *g, uint64 pad)
{
	TransactionId xid;
	uint8		kind;

	read_exact(g, (char *) &xid, sizeof(xid), pad + sizeof(uint32) + 1);
	if (TransactionIdIsInProgress(xid))
		return false;
	read_exact(g, (char *) &kind, 1, pad + sizeof(uint32));
	return kind == KIND_PAD;
}

/*
 * Settle the PADs of g seen in flight: one now a ROOT is the root (if it
 * is later than the one known), and one whose transaction ended a PAD is
 * forgotten.  The index's lock is held.
 */
static void
prune_pads(ProllyChunkStore *s, Segment *g, JournalSegScan *sg)
{
	JournalScan *js = jindex_scan(s->ix);
	uint64	   *pads = jindex_pads(s->ix, sg);

	for (int i = 0; i < sg->npads;)
	{
		uint64		pad = pads[i];
		uint8		kind;
		bool		done = false;

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
					if (root_later(js, g->id, pad))
						set_root(js, &h, g->id, pad);
					done = true;
				}
			}
		}
		else if (kind == KIND_PAD)
			done = pad_ended(g, pad);
		if (done)
			pads[i] = pads[--sg->npads];
		else
			i++;
	}
}

/*
 * Index a segment's records written since its last scan, for every backend
 * (the index's lock held exclusively): up to the first that isn't whole
 * yet.  A PAD seen may since have become a ROOT; one that stays a PAD once
 * its transaction has ended never will (it aborted), and is forgotten.
 */
static void
scan_segment(ProllyChunkStore *s, Segment *g)
{
	JournalSegScan *sg = jindex_segment_of(s->ix, g->id);
	JournalHole *holes;
	uint64		size;
	uint64		offset;
	char	   *rec = NULL;
	Size		reclen = 0;

	if (sg == NULL)
		elog(ERROR, "chunk journal segment %llu isn't in the shared index",
			 (unsigned long long) g->id);
	if (!open_segment(g))
		return;
	size = FileSize(g->file);
	holes = jindex_holes(s->ix, sg);
	offset = sg->scanned;

	prune_pads(s, g, sg);

	/* ranges skipped while their appends were in flight, if written since */
	for (int i = 0; i < sg->nholes;)
	{
		JournalHole *hole = &holes[i];
		uint64		end = hole->offset + hole->len;
		uint64		upto = index_range(s, g, sg, hole->offset, end, &rec, &reclen);

		if (upto == end)
			holes[i] = holes[--sg->nholes];
		else
		{
			/* (a write seen in part) */
			hole->len = end - upto;
			hole->offset = upto;
			i++;
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

		offset = index_range(s, g, sg, offset, size, &rec, &reclen);
		if (offset + RECORD_OVERHEAD > size)
			break;
		hole = append_in_flight(s, g->id, offset);
		if (hole > 0 && offset + hole <= size)
		{
			if (sg->nholes >= jindex_capacity(s->ix))
				elog(ERROR, "chunk journal segment %llu has too many appends in flight",
					 (unsigned long long) g->id);
			holes[sg->nholes].offset = offset;
			holes[sg->nholes].len = hole;
			sg->nholes++;
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
	sg->scanned = offset;
}

/*
 * Index the whole records of a segment in [offset, end), the first being
 * at offset; returns the offset of the first that isn't whole (end if
 * all are).  *rec and *reclen are a buffer to read into, in
 * TopMemoryContext, grown as needed.
 */
static uint64
index_range(ProllyChunkStore *s, Segment *g, JournalSegScan *sg,
			uint64 offset, uint64 end, char **rec, Size *reclen)
{
	JournalScan *js = jindex_scan(s->ix);
	uint64		start = offset;

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
			jindex_add(s->ix, &h, g->id, offset, len, xid);
		else if (kind == KIND_ROOT && record_hash(*rec, len, &h, &after))
		{
			if (root_later(js, g->id, offset))
				set_root(js, &h, g->id, offset);
		}
		else if (kind == KIND_PAD && !pad_ended(g, offset))
		{
			/* (full: those settled since they were seen let go first) */
			if (sg->npads >= jindex_capacity(s->ix))
				prune_pads(s, g, sg);
			if (sg->npads >= jindex_capacity(s->ix))
				elog(ERROR, "chunk journal segment %llu has too many roots in flight",
					 (unsigned long long) g->id);
			jindex_pads(s->ix, sg)[sg->npads++] = offset;
		}
		offset += len;
	}
	if (offset > start)
	{
		pgstat_count_prolly(PGSTAT_PROLLY_JOURNAL_SCANS, 1);
		pgstat_count_prolly(PGSTAT_PROLLY_JOURNAL_BYTES_SCANNED, offset - start);
	}
	return offset;
}

/* The root as the index's readers have it, copied to the store. */
static void
copy_root(ProllyChunkStore *s)
{
	JournalScan *js = jindex_scan(s->ix);

	s->root = js->root;
	s->root_segment = js->root_segment;
	s->root_offset = js->root_offset;
}

/*
 * Index what was written to the segments since they were last scanned, by
 * any backend.
 */
static void
scan_journal(ProllyChunkStore *s)
{
	JournalScan *js;

	refresh_segments(s);
	jindex_lock(s->ix, true);
	js = jindex_scan(s->ix);
	for (int i = 0; i < js->nsegments; i++)
		scan_segment(s, local_segment(s, jindex_segment(s->ix, i)->id));
	copy_root(s);
	jindex_unlock(s->ix);
}

/*
 * Take the segments a manifest names (one read when the manifests count
 * was count), in its order: those read before as they are, new ones to be
 * read from their start.  The shared index, if its own are older, takes
 * them too: it forgets the records of those no longer named (a collection
 * retired them), and a root read from one gives way to the manifest's.
 */
static void
load_segments(ProllyChunkStore *s, const Manifest *m, uint64 count)
{
	List	   *segs = NIL;
	List	   *dropped = NIL;
	ListCell   *lc;
	JournalScan *js;
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	for (int i = 0; i < m->nsegments; i++)
	{
		Segment    *g = segment_of(s, m->segments[i]);

		if (g == NULL)
		{
			foreach(lc, s->other_segments)
			{
				if (((Segment *) lfirst(lc))->id == m->segments[i])
				{
					g = lfirst(lc);
					s->other_segments = foreach_delete_current(s->other_segments, lc);
					break;
				}
			}
		}
		if (g == NULL)
		{
			g = palloc0(sizeof(Segment));
			g->id = m->segments[i];
			g->path = segment_path(s->dboid, s->spcoid, g->id);
			g->file = -1;
		}
		segs = lappend(segs, g);
	}
	foreach(lc, s->segments)
	{
		if (!list_member_ptr(segs, lfirst(lc)))
			dropped = lappend(dropped, lfirst(lc));
	}
	dropped = list_concat(dropped, s->other_segments);
	s->other_segments = NIL;
	MemoryContextSwitchTo(old);
	list_free(s->segments);
	s->segments = segs;
	s->gc_generation = m->gc_generation;
	s->base = m->base;

	jindex_lock(s->ix, true);
	js = jindex_scan(s->ix);
	if (js->manifests_seen < count)
	{
		bool		root_named = false;

		jindex_set_segments(s->ix, m->segments, m->nsegments,
							JOURNAL_HEADER_SIZE);
		for (int i = 0; i < m->nsegments; i++)
			root_named |= m->segments[i] == js->root_segment;
		if (js->root_offset == 0 || !root_named)
			set_root(js, &m->root, 0, 0);
		js->gc_generation = m->gc_generation;
		js->manifests_seen = count;
	}
	copy_root(s);
	jindex_unlock(s->ix);

	foreach(lc, dropped)
	{
		Segment    *g = lfirst(lc);

		if (g->file >= 0)
			FileClose(g->file);
		pfree(g->path);
		pfree(g);
	}
	list_free(dropped);
}

/* ----------------------------------------------------------------
 * Transactions
 * ----------------------------------------------------------------
 */

static bool tables_read(ProllyChunkStore *s, const ProllyHash *hash,
						StringInfo out, bool stored);
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
	if (jindex_has(s->ix, key) || tables_get(s, key, NULL))
		return true;
	scan_journal(s);
	if (jindex_has(s->ix, key))
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
	s->xact_flushes = s->flushes;
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
		trusted_has(s, &key) || stored_has(s, &key))
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

/*
 * The chunks a transaction appends as it commits (its pending ones, an
 * aborted subtransaction's gone from them), let into the cache: a backend
 * writing a little at a time (dolt.transaction_commit) reads first, in its
 * next transaction, what this one wrote last, the refs' and the trees'
 * upper nodes, each once before it is replaced.  Only a small
 * transaction's: none appended before (no flush), at most
 * PENDING_CACHED_MAX bytes and an eighth of the cache's, so a large one
 * doesn't push out what every backend reads, nor copy much as it commits;
 * and only if no collection swapped the store since the transaction began
 * (one dropped as stored already may be in a segment the swap removed).  Decoded before
 * the commit record, where allocating may fail, into the backend's memory,
 * and let into the shared cache once the transaction has committed
 * (pending_cached_end()), where nothing may fail and the cache's inserts
 * don't; so others never read from the cache a chunk the store may yet not
 * have.
 */
typedef struct StagedChunk
{
	ProllyHash	hash;
	Size		offset;			/* in staged */
	uint32		len;
} StagedChunk;

/* (enough for the refs' and a few trees' upper nodes a commit rewrites) */
#define PENDING_CACHED_MAX	(256 * 1024)

static StringInfo staged = NULL;	/* the chunks' bytes */
static StagedChunk *staged_chunks = NULL;
static int	nstaged = 0;
static uint32 staged_generation = 0;

static void
pending_cached(ProllyChunkStore *s)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;
	Size		budget = Min(chunk_cache_limit() / 8, PENDING_CACHED_MAX);
	Size		total = 0;
	long		n;
	MemoryContext old;

	nstaged = 0;
	if (s != session_store || budget == 0 ||
		MyProc->vxid.lxid != s->xact_lxid || s->flushes != s->xact_flushes ||
		s->gc_generation != s->xact_generation)
		return;
	n = hash_get_num_entries(s->pending_index);
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
		total += e->raw_len;
	if (n == 0 || total > budget)
		return;
	old = MemoryContextSwitchTo(TopMemoryContext);
	if (staged == NULL)
		staged = makeStringInfo();
	resetStringInfo(staged);
	if (staged_chunks != NULL)
		pfree(staged_chunks);
	staged_chunks = palloc(n * sizeof(StagedChunk));
	MemoryContextSwitchTo(old);
	staged_generation = s->gc_generation;
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		const char *rec = s->pending.data + e->offset;
		ProllyHash	h;
		Size		after;
		const char *chunk = NULL;
		Size		chunk_len = 0;

		if (record_hash(rec, e->len, &h, &after))
			chunk = record_chunk(rec, e->len, after, chunk_scratch(), &chunk_len);
		if (chunk == NULL)
			continue;
		staged_chunks[nstaged].hash = e->hash;
		staged_chunks[nstaged].offset = staged->len;
		staged_chunks[nstaged].len = (uint32) chunk_len;
		appendBinaryStringInfo(staged, chunk, chunk_len);
		nstaged++;
	}
}

/* The transaction ended: what it staged let into the cache if it committed. */
static void
pending_cached_end(bool commit)
{
	if (commit && session_store != NULL)
	{
		for (int i = 0; i < nstaged; i++)
			chunk_cache_put(session_store, &staged_chunks[i].hash,
							staged->data + staged_chunks[i].offset,
							staged_chunks[i].len, staged_generation, true);
	}
	nstaged = 0;
	/* (a large one's memory let go of) */
	if (staged != NULL && staged->maxlen > CHUNK_SCRATCH_KEEP)
	{
		destroyStringInfo(staged);
		staged = NULL;
	}
}

/*
 * The pending chunks are in segment g from base on: index them there (with
 * index; a commit's are found by the next scan, as nothing may fail as the
 * transaction ends).
 */
static void
pending_appended(ProllyChunkStore *s, Segment *g, uint64 base, bool index)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		if (index)
			jindex_add(s->ix, &e->hash, g->id, base + e->offset, e->len, e->xid);
		/* (counted once appended: those already stored were dropped) */
		pgstat_count_prolly(PGSTAT_PROLLY_CHUNKS_APPENDED, 1);
		pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_BYTES_APPENDED, e->raw_len);
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

static const ProllyTableFileRoutine *table_format(void);

static int
pending_hash_cmp(const void *a, const void *b)
{
	const IndexEntry *x = *(IndexEntry *const *) a;
	const IndexEntry *y = *(IndexEntry *const *) b;
	int			c = memcmp(x->hash.bytes, y->hash.bytes, Min(x->hash.len, y->hash.len));

	return c != 0 ? c : (int) x->hash.len - (int) y->hash.len;
}

static int
pending_offset_cmp(const void *a, const void *b)
{
	const IndexEntry *x = *(IndexEntry *const *) a;
	const IndexEntry *y = *(IndexEntry *const *) b;

	return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

/*
 * Before the pending chunks are appended: those the store has already
 * dropped, asked all at once, in hash order (the shared index, then each
 * table file, its filter first), rather than as each was put.  Only
 * before a root's PAD is added to pending.  The rest are moved down over
 * them, in their order, and each subtransaction's saved length of pending
 * (since the last append) made the length of what is kept of it.
 */
static void
drop_stored(ProllyChunkStore *s)
{
	long		n = hash_get_num_entries(s->pending_index);
	IndexEntry **es;
	bool	   *stored;
	int			nstored = 0;
	HASH_SEQ_STATUS status;
	IndexEntry *e;
	int			k = 0;
	ListCell   *lc;

	if (n == 0)
		return;
	refresh_segments(s);
	load_tables(s, false);
	es = palloc(sizeof(IndexEntry *) * n);
	stored = palloc0(sizeof(bool) * n);
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
		es[k++] = e;
	qsort(es, n, sizeof(IndexEntry *), pending_hash_cmp);
	for (int i = 0; i < n; i++)
	{
		stored[i] = jindex_has(s->ix, &es[i]->hash);
		nstored += stored[i];
	}
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);

		for (int i = 0; i < n; i++)
		{
			uint64		offset;
			uint32		len;

			if (!stored[i] &&
				PROLLY_CALL(table_format(), find, t->tf, &es[i]->hash, &offset, &len))
			{
				stored[i] = true;
				nstored++;
			}
		}
	}
	if (nstored > 0)
	{
		ProllyHash *gone = palloc(sizeof(ProllyHash) * nstored);
		int			ngone = 0;
		int			to = 0;
		ListCell   *sc;

		for (int i = 0; i < n; i++)
			if (stored[i])
				gone[ngone++] = es[i]->hash;
		k = 0;
		for (int i = 0; i < n; i++)
			if (!stored[i])
				es[k++] = es[i];
		qsort(es, k, sizeof(IndexEntry *), pending_offset_cmp);

		/*
		 * A save's length: of the bytes kept, those before it (pending only
		 * grows between appends, so a chunk was there at the save if its
		 * offset was before the length saved).
		 */
		foreach(sc, s->pending_saved)
		{
			PendingSave *save = lfirst(sc);
			int			len = 0;

			if (save->flushes != s->flushes)
				continue;
			for (int i = 0; i < k && es[i]->offset < save->len; i++)
				len += es[i]->len;
			save->len = len;
		}
		/* (in place: in offset order, each moves down, if at all) */
		for (int i = 0; i < k; i++)
		{
			memmove(s->pending.data + to, s->pending.data + es[i]->offset,
					es[i]->len);
			es[i]->offset = to;
			to += es[i]->len;
		}
		s->pending.len = to;
		s->pending.data[to] = '\0';
		for (int i = 0; i < ngone; i++)
			hash_search(s->pending_index, &gone[i], HASH_REMOVE, NULL);
		pfree(gone);
	}
	pfree(es);
	pfree(stored);
}

/* Append the transaction's pending chunks. */
static void
flush_pending(ProllyChunkStore *s)
{
	Append		a;

	drop_stored(s);
	if (s->pending.len == 0)
		return;
	reserve_pending(s, &a, NULL);
	append_write(&a, s->pending.data);
	pending_appended(s, a.g, a.offset, true);
}

/*
 * The root a move goes to has its PAD at offset of segment g, in an append
 * reserved at time at: keep the ROOT record that takes its place as the
 * append is written (committed()).
 */
static void
root_padded(ProllyChunkStore *s, Segment *g, uint64 offset, TimestampTz at)
{
	StringInfoData rec;

	s->pad_segment = g;
	s->pad_offset = offset;
	INJECTION_POINT("prolly-root-padded", NULL);
	initStringInfo(&rec);
	s->root_at = at;
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

	drop_stored(s);
	if (s->pending.len == 0 && !s->moving)
		return;
	reserve_pending(s, &s->commit_append, s->moving ? &pad_at : NULL);
	s->at_commit = true;
	INJECTION_POINT("prolly-append-reserved", NULL);
	if (s->moving)
		root_padded(s, s->commit_append.g, s->commit_append.offset + pad_at,
					s->commit_append.at);
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

static void ahead_forget(ProllyChunkStore *s);

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
			drop_stored(s);
			if (s->pending.len > 0 || s->moving)
			{
				Append		a;
				int			pad_at = -1;

				reserve_pending(s, &a, s->moving ? &pad_at : NULL);
				append_write(&a, s->pending.data);
				pending_appended(s, a.g, a.offset, true);
				if (s->moving)
					root_padded(s, a.g, a.offset + pad_at, a.at);
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
			/* (what it let into the cache stored now: a prepared one's not) */
			pending_cached_end(event != XACT_EVENT_PREPARE);
			if (s->commit_append.g != NULL)
			{
				pending_appended(s, s->commit_append.g, s->commit_append.offset,
								 false);
				s->commit_append.g = NULL;
			}
			s->moving = false;
			s->pad_offset = 0;
			s->saved = NIL;
			s->pending_saved = NIL;
			s->through = NULL;
			s->through_hint = NULL;
			ahead_forget(s);
			s->transient = NULL;	/* (with the transaction's memory) */
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			pending_cached_end(false);
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
			ahead_forget(s);
			s->transient = NULL;
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
		jindex_append_lock(s->ix, true);
		segment_header(s->dboid, s->spcoid, g->id);
		jindex_append_unlock(s->ix);
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
	if (id & RECEIVED_ID)
		return psprintf(RECEIVED_PREFIX "%016llx",
						(unsigned long long) (id & ~RECEIVED_ID));
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
	append_le(out, m->base.len, 1);
	appendBinaryStringInfo(out, m->base.bytes, m->base.len);
	append_le(out, m->nfiles, 4);
	for (int i = 0; i < m->nfiles; i++)
	{
		append_le(out, m->files[i].id, 8);
		append_le(out, m->files[i].nchunks, 4);
		append_le(out, m->files[i].size, 8);
		append_le(out, m->files[i].flags, 1);
		append_le(out, m->files[i].epoch_lo, 4);
		append_le(out, m->files[i].epoch_hi, 4);
		append_le(out, m->files[i].garbage, 4);
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
	append_le(out, m->nepochs, 4);
	for (int i = 0; i < m->nepochs; i++)
	{
		append_le(out, m->epochs[i].epoch, 4);
		append_le(out, (uint64) m->epochs[i].ended, 8);
	}
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
	uint32		nepochs;

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
	if (st.st_size < 8 + 4 + FORMAT_SIZE + 4 + 8 + 1 + 1 + 4 + 4 + 4 + 4 + 4)
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
	rootlen = (uint8) *p++;
	if (rootlen > PROLLY_HASH_MAX_LEN || end - p < rootlen + 4)
		goto corrupt;
	m->base.len = rootlen;
	memcpy(m->base.bytes, p, rootlen);
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
		m->files[i].epoch_lo = (uint32) read_le(p + 21, 4);
		m->files[i].epoch_hi = (uint32) read_le(p + 25, 4);
		m->files[i].garbage = (uint32) read_le(p + 29, 4);
		if (m->files[i].epoch_lo > m->files[i].epoch_hi ||
			((m->files[i].flags & MANIFEST_FILE_REPAIRED) != 0 &&
			 (m->files[i].flags & MANIFEST_FILE_SETTLED) == 0) ||
			((m->files[i].flags & MANIFEST_FILE_SETTLED) != 0) !=
			(m->files[i].epoch_lo > 0))
			goto corrupt;
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
	if ((Size) (end - p) < (Size) nretiring * 8 + 4)
		goto corrupt;
	m->nretiring = nretiring;
	m->retiring = palloc(sizeof(uint64) * Max(nretiring, 1));
	for (uint32 i = 0; i < nretiring; i++)
	{
		m->retiring[i] = read_le(p, 8);
		p += 8;
	}
	nepochs = (uint32) read_le(p, 4);
	p += 4;
	if ((Size) (end - p) != (Size) nepochs * MANIFEST_EPOCH_ENTRY)
		goto corrupt;
	m->nepochs = nepochs;
	m->epochs = palloc(sizeof(ManifestEpoch) * Max(nepochs, 1));
	for (uint32 i = 0; i < nepochs; i++)
	{
		m->epochs[i].epoch = (uint32) read_le(p, 4);
		m->epochs[i].ended = (TimestampTz) read_le(p + 4, 8);
		if (i > 0 && m->epochs[i].epoch <= m->epochs[i - 1].epoch)
			goto corrupt;
		p += MANIFEST_EPOCH_ENTRY;
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
	load_segments(s, &m, count);
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
	bool		refused;

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.id = id;
	xlrec.offset = offset;

	refused = sync_refused();
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
	request_sync(s->dboid, s->spcoid, SYNC_TABLE_FILE, id, refused);
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
}

/* Add a source to run; bytes that run on from the last's extend it. */
static void
copy_add(CopyRun *run, const CopySource *src)
{
	CopySource *last = run->n > 0 ? &run->sources[run->n - 1] : NULL;

	if (last != NULL && src->kind != COPY_SEGMENT_CHUNK &&
		last->kind == src->kind && last->id == src->id &&
		last->offset + last->len == src->offset &&
		(uint64) last->len + src->len <= PG_UINT32_MAX)
	{
		last->len += src->len;
		return;
	}
	if (run->n == run->max)
	{
		run->max = Max(64, run->max * 2);
		run->sources = run->sources ?
			repalloc(run->sources, sizeof(CopySource) * run->max) :
			palloc(sizeof(CopySource) * run->max);
	}
	run->sources[run->n++] = *src;
}

/*
 * Write bytes copied from files of the store (or of CREATE DATABASE's
 * template), run's sources, at offset of table file id (segment: of a
 * segment), WAL-logged first as where they come from and their CRC, not
 * as the bytes (#564): redo copies them again from its own files, which
 * hold the same bytes (every byte of a store's files is the WAL's).
 *
 * The bytes are logged instead with prolly.log_copied_bytes, or (in_backup)
 * while an online backup runs: for a copy whose sources a later DROP
 * DATABASE may remove before the backup reads them.  That is asked under
 * DELAY_CHKPT_START: a backup that starts later has its start checkpoint
 * after the record, waiting for the bytes to be written, so they are in it.
 */
static void
copy_write(Oid dboid, Oid spcoid, bool segment, uint64 id, uint64 offset,
		   const char *data, Size len, const CopyRun *run, bool in_backup,
		   int fd, const char *path)
{
	xl_chunks_copy xlrec;
	xl_chunks_file frec;
	xl_chunks_append arec;
	char	   *sources = palloc(COPY_SOURCE_SIZE * Max(run->n, 1));
	char	   *p = sources;
	XLogRecPtr	lsn;
	bool		refused;

	Assert(run->n > 0 && len <= PG_UINT32_MAX);
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = dboid;
	xlrec.spcoid = spcoid;
	xlrec.id = id;
	xlrec.offset = offset;
	xlrec.src_dboid = run->src_dboid;
	xlrec.src_spcoid = run->src_spcoid;
	xlrec.segment = segment;
	xlrec.len = (uint32) len;
	INIT_CRC32C(xlrec.crc);
	COMP_CRC32C(xlrec.crc, data, len);
	FIN_CRC32C(xlrec.crc);
	xlrec.nsources = run->n;
	for (int i = 0; i < run->n; i++)
	{
		const CopySource *src = &run->sources[i];

		*p = (char) src->kind;
		memcpy(p + 1, &src->id, sizeof(uint64));
		memcpy(p + 9, &src->offset, sizeof(uint64));
		memcpy(p + 17, &src->len, sizeof(uint32));
		p += COPY_SOURCE_SIZE;
	}
	/* (the records the bytes would be logged in otherwise) */
	frec.hdr = arec.hdr = xlrec.hdr;
	frec.dboid = arec.dboid = dboid;
	frec.spcoid = arec.spcoid = spcoid;
	frec.id = arec.segment = id;
	frec.offset = arec.offset = offset;

	refused = sync_refused();
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	XLogBeginInsert();
	if (prolly_log_copied_bytes || (in_backup && XLogBackupInProgress()))
	{
		if (segment)
			XLogRegisterData(&arec, SizeOfChunksAppend);
		else
			XLogRegisterData(&frec, SizeOfChunksFile);
		XLogRegisterData(data, len);
		lsn = XLogInsert(RM_PROLLY_CHUNKS_ID,
						 segment ? XLOG_CHUNKS_APPEND : XLOG_CHUNKS_FILE);
	}
	else
	{
		XLogRegisterData(&xlrec, SizeOfChunksCopy);
		XLogRegisterData(sources, COPY_SOURCE_SIZE * run->n);
		lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_COPY);
	}
	XLogFlush(lsn);
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (pg_pwrite(fd, data, len, offset) != len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, path)));
	pgstat_report_wait_end();
	pgstat_count_prolly(segment ? PGSTAT_PROLLY_JOURNAL_BYTES_WRITTEN :
						PGSTAT_PROLLY_TABLE_FILE_BYTES_WRITTEN, len);
	request_sync(dboid, spcoid, segment ? SYNC_SEGMENT : SYNC_TABLE_FILE, id,
				 refused);
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
	pfree(sources);
}

/*
 * Remove a store file, which a copy may have copied from (#564): not while
 * an online backup runs, which may not have read it yet, and whose redo
 * would copy from it.  So asked once the removal is logged: a backup that
 * starts later has its start point after the record, and after every copy
 * from the file.  A file left is named no longer: the next writer's
 * remove_unnamed() takes it, or, on a standby, redo once the backups end
 * (or, a restart having forgotten it, as redo reaches a consistent state:
 * remove_dropped()).
 */
static void
remove_store_file(Oid dboid, Oid spcoid, bool segment, uint64 id,
				  const char *path)
{
	if (XLogBackupInProgress())
	{
		if (InRecovery)
			defer_unlink(dboid, spcoid, segment, id);
		return;
	}
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
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
	remove_store_file(s->dboid, s->spcoid, segment, id, path);
	pfree(path);
}

static bool
table_file_read(void *arg, uint64 offset, char *buf, Size len)
{
	TableFile  *t = arg;

	return FileRead(t->file, buf, len, offset, WAIT_EVENT_DATA_FILE_READ) == len;
}

/*
 * Map len bytes of a table file at offset for its format to read in place:
 * every process that has the file open shares the one copy in the page
 * cache.  A table file is never cut short once a manifest names it (redo
 * writes a named one's bytes over in place); one a conjoin or collection
 * removes stays mapped until closed.  A read of the mapping the OS can't
 * do (an I/O error) is a SIGBUS, not an error: the process dies and the
 * server restarts, as for any of its shared mappings, where read() would
 * have raised an error.
 */
static const char *
table_file_map(void *arg, uint64 offset, Size len)
{
#ifndef WIN32
	TableFile  *t = arg;
	uint64		page = (uint64) sysconf(_SC_PAGESIZE);
	uint64		start = offset - offset % page;
	void	   *p;

	if (len == 0 || t->mapped != NULL)
		return NULL;
	p = mmap(NULL, len + (offset - start), PROT_READ, MAP_SHARED,
			 FileGetRawDesc(t->file), (off_t) start);
	if (p == MAP_FAILED)
		return NULL;
	t->mapped = p;
	t->mapped_len = len + (offset - start);
	return (const char *) p + (offset - start);
#else
	return NULL;
#endif
}

/* Close a table file's format, mapping and file (not its memory). */
static void
release_table(TableFile *t)
{
	if (t->tf)
		PROLLY_CALL(table_format(), close, t->tf);
	t->tf = NULL;
#ifndef WIN32
	if (t->mapped)
		munmap(t->mapped, t->mapped_len);
#endif
	t->mapped = NULL;
	if (t->file >= 0)
		FileClose(t->file);
	t->file = -1;
}

static void
close_table(TableFile *t)
{
	release_table(t);
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
	t->epoch_lo = f->epoch_lo;
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
	/* (a format that raises an error has its mapping and file closed) */
	PG_TRY();
	{
		t->tf = PROLLY_CALL(table_format(), open, table_file_read,
							table_file_map, t, t->size, cxt);
	}
	PG_CATCH();
	{
		t->tf = NULL;
		release_table(t);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (t->tf == NULL)
	{
		const ProllyTableFileRoutine *format = table_format();
		uint32		version = format->version == NULL ? 0 :
			format->version(table_file_read, t, t->size);

#ifndef WIN32
		if (t->mapped)
			munmap(t->mapped, t->mapped_len);
#endif
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
			load_segments(s, &m, count);
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
	return tables_read(s, hash, out, false);
}

/* A chunk of a table file: decoded, or (stored) framed as stored. */
static bool
tables_read(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out,
			bool stored)
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
			pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_FILE_READS, 1);
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
			if (stored)
				prolly_tablefile_read_stored(table_format(), t->tf, bytes, len, out);
			else
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

/* ------------------------------------------------------------------------
 * Generations: what a settled file's chunks name in older ones
 * ------------------------------------------------------------------------
 *
 * A settled table file's section (the table file format's write_section())
 * lists the chunks its chunks name that are in a file of an older
 * generation, each once:
 *
 *		uint8 version (1), uint8 hash length, uint32 count of epochs, then
 *		for each epoch, ascending: uint32 epoch, uint32 count of targets,
 *		uint32 count of pairs; then for each epoch in that order its
 *		targets (hashes, ascending), then its pairs (a leaf naming a
 *		target: the leaf's hash, then the target's, ascending by target)
 *
 * An epoch is one of the generations of the file the target was found in.
 * What a leaf names is listed with the leaf, so that a collection can
 * leave out what the history horizon cuts (a boundary commit's parents).
 */
#define XREF_VERSION		1
#define XREF_HEAD			6
#define XREF_EPOCH_ENTRY	12

/* a target, as a file's chunks name it */
typedef struct Xref
{
	uint32		epoch;
	ProllyHash	target;
	ProllyHash	source;			/* the leaf naming it (len 0: not a leaf) */
} Xref;

static int
xref_cmp(const void *a, const void *b)
{
	const Xref *x = a;
	const Xref *y = b;
	int			c;

	if (x->epoch != y->epoch)
		return x->epoch < y->epoch ? -1 : 1;
	if ((x->source.len > 0) != (y->source.len > 0))
		return x->source.len > 0 ? 1 : -1;
	c = memcmp(x->target.bytes, y->target.bytes, x->target.len);
	if (c != 0)
		return c;
	return memcmp(x->source.bytes, y->source.bytes, x->source.len);
}

/* The section of xrefs (n of them, deduplicated here), appended to out. */
static void
xrefs_encode(Xref *xrefs, int n, uint8 hash_len, StringInfo out)
{
	int			nepochs = 0;
	int			k = 0;

	if (n == 0)
		return;
	qsort(xrefs, n, sizeof(Xref), xref_cmp);
	/* (each once) */
	for (int i = 0; i < n; i++)
	{
		if (k > 0 && xref_cmp(&xrefs[k - 1], &xrefs[i]) == 0)
			continue;
		xrefs[k++] = xrefs[i];
	}
	n = k;
	for (int i = 0; i < n; i++)
		nepochs += (i == 0 || xrefs[i].epoch != xrefs[i - 1].epoch);
	appendStringInfoChar(out, XREF_VERSION);
	appendStringInfoChar(out, (char) hash_len);
	append_le(out, nepochs, 4);
	for (int i = 0; i < n;)
	{
		int			j = i;
		uint32		plain = 0;
		uint32		pairs = 0;

		while (j < n && xrefs[j].epoch == xrefs[i].epoch)
		{
			if (xrefs[j].source.len > 0)
				pairs++;
			else
				plain++;
			j++;
		}
		append_le(out, xrefs[i].epoch, 4);
		append_le(out, plain, 4);
		append_le(out, pairs, 4);
		i = j;
	}
	for (int i = 0; i < n; i++)
	{
		if (xrefs[i].source.len > 0)
			appendBinaryStringInfo(out, (char *) xrefs[i].source.bytes, hash_len);
		appendBinaryStringInfo(out, (char *) xrefs[i].target.bytes, hash_len);
	}
}

/* A section, read: its epochs' places in its bytes. */
typedef struct XrefSection
{
	char	   *data;
	Size		len;
	uint8		hash_len;
	int			nepochs;
	uint32	   *epochs;
	uint32	   *nplain;
	uint32	   *npairs;
	Size	   *at;				/* where each epoch's targets begin */
} XrefSection;

static void
xref_corrupt(const char *path)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the generations section of table file \"%s\" is corrupt",
					path)));
}

/*
 * Table file t's section, read and checked, into x; false if it has none.
 */
static bool
xrefs_read(TableFile *t, XrefSection *x)
{
	uint64		offset;
	uint32		len;
	uint32		crc;
	pg_crc32c	got;
	Size		at;

	memset(x, 0, sizeof(*x));
	PROLLY_CALL(table_format(), section, t->tf, &offset, &len, &crc);
	if (len == 0)
		return false;
	x->data = palloc(len);
	x->len = len;
	if (!table_file_read(t, offset, x->data, len))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, t->path)));
	INIT_CRC32C(got);
	COMP_CRC32C(got, x->data, len);
	FIN_CRC32C(got);
	if (!EQ_CRC32C(got, crc) || len < XREF_HEAD ||
		(uint8) x->data[0] != XREF_VERSION)
		xref_corrupt(t->path);
	x->hash_len = (uint8) x->data[1];
	x->nepochs = (int) read_le(x->data + 2, 4);
	if (x->hash_len == 0 || x->hash_len > PROLLY_HASH_MAX_LEN ||
		x->nepochs < 0 ||
		(len - XREF_HEAD) / XREF_EPOCH_ENTRY < (Size) x->nepochs)
		xref_corrupt(t->path);
	x->epochs = palloc(sizeof(uint32) * Max(x->nepochs, 1));
	x->nplain = palloc(sizeof(uint32) * Max(x->nepochs, 1));
	x->npairs = palloc(sizeof(uint32) * Max(x->nepochs, 1));
	x->at = palloc(sizeof(Size) * Max(x->nepochs, 1));
	at = XREF_HEAD + (Size) x->nepochs * XREF_EPOCH_ENTRY;
	for (int i = 0; i < x->nepochs; i++)
	{
		const char *e = x->data + XREF_HEAD + (Size) i * XREF_EPOCH_ENTRY;

		x->epochs[i] = (uint32) read_le(e, 4);
		x->nplain[i] = (uint32) read_le(e + 4, 4);
		x->npairs[i] = (uint32) read_le(e + 8, 4);
		if (i > 0 && x->epochs[i] <= x->epochs[i - 1])
			xref_corrupt(t->path);
		x->at[i] = at;
		at += ((Size) x->nplain[i] + 2 * (Size) x->npairs[i]) * x->hash_len;
		if (at > len)
			xref_corrupt(t->path);
	}
	if (at != len)
		xref_corrupt(t->path);
	return true;
}

static void
xrefs_free(XrefSection *x)
{
	if (x->data == NULL)
		return;
	pfree(x->data);
	pfree(x->epochs);
	pfree(x->nplain);
	pfree(x->npairs);
	pfree(x->at);
}

/*
 * Visit the targets of a section's epochs from lo to hi: each with the
 * leaf naming it (NULL: not a leaf's).
 */
typedef void (*XrefVisit) (const ProllyHash *target, const ProllyHash *source,
						   uint32 epoch, void *arg);

static void
xrefs_visit(const XrefSection *x, uint32 lo, uint32 hi, XrefVisit visit,
			void *arg)
{
	for (int i = 0; i < x->nepochs; i++)
	{
		const char *p;
		ProllyHash	target;
		ProllyHash	source;

		if (x->epochs[i] < lo || x->epochs[i] > hi)
			continue;
		memset(&target, 0, sizeof(target));
		memset(&source, 0, sizeof(source));
		target.len = source.len = x->hash_len;
		p = x->data + x->at[i];
		for (uint32 j = 0; j < x->nplain[i]; j++)
		{
			memcpy(target.bytes, p, x->hash_len);
			p += x->hash_len;
			visit(&target, NULL, x->epochs[i], arg);
		}
		for (uint32 j = 0; j < x->npairs[i]; j++)
		{
			memcpy(source.bytes, p, x->hash_len);
			memcpy(target.bytes, p + x->hash_len, x->hash_len);
			p += 2 * x->hash_len;
			visit(&target, &source, x->epochs[i], arg);
		}
	}
}

/*
 * The generation a chunk a file's chunks name is in, as the file's writer
 * resolves it: the first of the interval of the file it is in (or will be
 * in), or 0 if none it knows of has it.  *always says to list it even if
 * it is of the file's own generation: a parent the history horizon severs,
 * which a repair brings back in that generation (see settle()).
 */
typedef uint32 (*XrefResolve) (void *arg, const ProllyHash *target,
							   bool *always);

/* What a file being written lists in its section, as its chunks come. */
typedef struct XrefBuild
{
	XrefResolve resolve;
	void	   *arg;
	uint32		lo;				/* the file's generations */
	uint32		hi;
	Xref	   *items;
	int			n;
	int			max;
	HTAB	   *seen;			/* the items, each once (NULL: none yet) */
	ProllyHash	source;			/* the chunk being added */
	bool		leaf;			/* and whether it is a leaf */
	StringInfoData chunk;		/* (its bytes, unframed) */
} XrefBuild;

static void
xref_note(const ProllyHash *ref, void *arg)
{
	XrefBuild  *xb = arg;
	uint32		epoch;
	Xref	   *x;

	bool		always = false;

	if (ref->len == 0)
		return;
	epoch = xb->resolve(xb->arg, ref, &always);
	/* (one of the file's own generations, or found nowhere: not listed) */
	if (epoch == 0 || (epoch >= xb->lo && !always))
		return;
	{
		Xref		item;
		bool		found;

		memset(&item, 0, sizeof(item));
		item.epoch = epoch;
		memcpy(item.target.bytes, ref->bytes, ref->len);
		item.target.len = ref->len;
		if (xb->leaf)
		{
			memcpy(item.source.bytes, xb->source.bytes, xb->source.len);
			item.source.len = xb->source.len;
		}
		if (xb->seen == NULL)
		{
			HASHCTL		ctl;

			ctl.keysize = sizeof(Xref);
			ctl.entrysize = sizeof(Xref);
			ctl.hcxt = CurrentMemoryContext;
			xb->seen = hash_create("prolly section items", 1024, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}
		(void) hash_search(xb->seen, &item, HASH_ENTER, &found);
		if (found)
			return;
		if (xb->n == xb->max)
		{
			xb->max = Max(64, xb->max * 2);
			xb->items = xb->items ?
				repalloc_huge(xb->items, sizeof(Xref) * xb->max) :
				MemoryContextAllocHuge(CurrentMemoryContext, sizeof(Xref) * xb->max);
		}
		x = &xb->items[xb->n++];
		*x = item;
	}
}

/* A chunk added to the file xb builds the section of: what it names. */
static void
xref_add(XrefBuild *xb, const ProllyHash *hash, const char *data, Size len,
		 bool stored)
{
	if (stored)
	{
		resetStringInfo(&xb->chunk);
		if (!prolly_chunk_unframe(data, len, &xb->chunk))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("chunk %s isn't framed as stored chunks are",
							prolly_hash_hex(hash))));
		data = xb->chunk.data;
		len = xb->chunk.len;
	}
	xb->source = *hash;
	xb->leaf = prolly_chunk_is_leaf(data, len);
	prolly_chunk_references(data, len, xref_note, xb);
}

/* a target a section lists, and the generation it lists it by */
typedef struct ListedTarget
{
	ProllyHash	target;			/* key */
	uint32		epoch;
} ListedTarget;

static void
note_listed_target(const ProllyHash *target, const ProllyHash *source,
				   uint32 epoch, void *arg)
{
	HTAB	   *listed = arg;
	ProllyHash	key;
	ListedTarget *lt;
	bool		found;

	hash_key(&key, target);
	lt = hash_search(listed, &key, HASH_ENTER, &found);
	if (!found || epoch > lt->epoch)
		lt->epoch = epoch;
}

/*
 * The generation table file t's section lists the chunk (key) by, 0 if it
 * doesn't, *listed caching what it lists (read once, in cxt).  For a chunk
 * gone from the store, which a chunk of t names: a severed parent, listed
 * when its file was written, which stays listed by the generation a repair
 * puts it back in.
 */
static uint32
listed_epoch(TableFile *t, HTAB **listed, MemoryContext cxt,
			 const ProllyHash *key, bool *always)
{
	ListedTarget *lt;

	if (t == NULL)
		return 0;
	if (*listed == NULL)
	{
		HASHCTL		ctl;
		XrefSection x;
		MemoryContext old = MemoryContextSwitchTo(cxt);

		ctl.keysize = sizeof(ProllyHash);
		ctl.hash = chunk_hash_key_hash;
		ctl.entrysize = sizeof(ListedTarget);
		ctl.hcxt = cxt;
		*listed = hash_create("prolly listed targets", 64, &ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
		if (xrefs_read(t, &x))
		{
			xrefs_visit(&x, 0, PG_UINT32_MAX, note_listed_target, *listed);
			xrefs_free(&x);
		}
		MemoryContextSwitchTo(old);
	}
	lt = hash_search(*listed, key, HASH_FIND, NULL);
	if (lt == NULL)
		return 0;
	*always = true;
	return lt->epoch;
}

/* How many targets file f's section lists (-1: the file is gone). */
static int64
file_xrefs(const char *dir, const ManifestFile *f)
{
	TableFile  *t = open_table(dir, f, CurrentMemoryContext);
	XrefSection x;
	int64		n = 0;

	if (t == NULL)
		return -1;
	if (xrefs_read(t, &x))
	{
		for (int i = 0; i < x.nepochs; i++)
			n += x.nplain[i] + x.npairs[i];
		xrefs_free(&x);
	}
	close_table(t);
	return n;
}

/*
 * Bytes of table file id written at offset: those copied from the store's
 * files (run's sources, if it has any) logged as a copy, the others as
 * bytes; run emptied.
 */
static void
file_write(ProllyChunkStore *s, int fd, const char *path, uint64 id,
		   uint64 offset, StringInfo buf, CopyRun *run)
{
	if (run->n > 0)
		copy_write(s->dboid, s->spcoid, false, id, offset, buf->data, buf->len,
				   run, false, fd, path);
	else
		table_file_write(s, fd, path, id, offset, buf->data, buf->len);
	run->n = 0;
}

/*
 * Did a chunk's bytes as written (bytes, len) come from where from says,
 * so that redo can make them again from there?  A record's chunk added as
 * stored is (the format's bytes for a frame are the frame's alone); a
 * table file's bytes are if they are the same bytes.
 */
static bool
copy_describes(const CopyFrom *from, const char *bytes, Size len)
{
	if (from->src.kind == COPY_SEGMENT_CHUNK)
		return true;
	return from->src.kind == COPY_TABLE_BYTES && from->src.len == len &&
		memcmp(from->bytes, bytes, len) == 0;
}

/*
 * Write table file id of the chunks next() gives (a chunk given twice
 * once), and sync it; false, writing nothing, if it gives none.  With a
 * limit, no more is asked of next() once the file's bytes reach it (*full
 * set): the caller begins another.  If from is given, next() sets it for
 * each chunk it gives (framed, stored): where it is in the store's files,
 * if it is; what the file copies from them is WAL-logged as copies (#564).
 */
static bool
write_file(ProllyChunkStore *s, uint64 id, ProllyChunkNext next, void *arg,
		   bool stored, CopyFrom *from, uint64 limit, XrefBuild *xb,
		   bool *full, uint32 *nchunks_out, uint64 *size_out)
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
	CopyRun		run = {s->dboid, s->spcoid, 0, 0, NULL};
	bool		copied = false; /* buf's bytes are copies */

	/* (a format adding a frame some other way may compress it again) */
	if (!stored || !PROLLY_HAS(fmt, write_add_stored))
		from = NULL;
	*full = false;
	initStringInfo(&data);
	if (!next(arg, &hash, &data))
	{
		pfree(data.data);
		pfree(path);
		pfree(dir);
		return false;
	}
	ctl.keysize = sizeof(ProllyHash);
	ctl.hash = chunk_hash_key_hash;
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly table file chunks", 1024, &ctl,
					   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

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
		int			start = buf.len;
		bool		copy;

		/* a chunk given twice is written once */
		hash_key(&key, &hash);
		(void) hash_search(seen, &key, HASH_ENTER, &found);
		if (found)
		{
			resetStringInfo(&data);
			continue;
		}
		if (stored)
			prolly_tablefile_add_stored(fmt, w, &hash, data.data, data.len, &buf);
		else
			PROLLY_CALL(fmt, write_add, w, &hash, data.data, data.len, &buf);
		nchunks++;
		if (xb)
			xref_add(xb, &hash, data.data, data.len, stored);

		/* (copies and bytes logged apart: the bytes before written first) */
		copy = from != NULL && from->src.kind != 0 &&
			copy_describes(from, buf.data + start, buf.len - start);
		if (copy != copied && start > 0)
		{
			int			added = buf.len - start;

			buf.len = start;
			file_write(s, fd, path, id, offset, &buf, &run);
			offset += start;
			memmove(buf.data, buf.data + start, added);
			buf.len = added;
		}
		copied = copy;
		if (copy)
			copy_add(&run, &from->src);
		if (buf.len >= APPEND_BATCH)
		{
			file_write(s, fd, path, id, offset, &buf, &run);
			offset += buf.len;
			resetStringInfo(&buf);
		}
		resetStringInfo(&data);
		CHECK_FOR_INTERRUPTS();
		/* (the bytes as written: compressed, framed) */
		*full = limit > 0 && offset + buf.len >= limit;
	} while (!*full && next(arg, &hash, &data));
	if (copied && buf.len > 0)
	{
		file_write(s, fd, path, id, offset, &buf, &run);
		offset += buf.len;
		resetStringInfo(&buf);
	}
	if (xb && xb->n > 0)
	{
		StringInfoData section;

		initStringInfo(&section);
		xrefs_encode(xb->items, xb->n, hash.len, &section);
		PROLLY_CALL(fmt, write_section, w, section.data, section.len);
		pfree(section.data);
	}
	if (xb)
	{
		xb->n = 0;
		if (xb->seen)
			hash_destroy(xb->seen);
		xb->seen = NULL;
	}
	PROLLY_CALL(fmt, write_finish, w, &buf);
	file_write(s, fd, path, id, offset, &buf, &run);
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
	if (run.sources)
		pfree(run.sources);
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
 * so (from, as write_file()'s).  The manifest's lock is held.
 */
static void
write_table_file(ProllyChunkStore *s, Manifest *m, bool *keep, uint8 flags,
				 uint32 epoch_lo, uint32 epoch_hi,
				 ProllyChunkNext next, void *arg, bool stored, CopyFrom *from,
				 bool copies, uint64 limit, XrefBuild *xb)
{
	char	   *dir;
	uint64		id = m->last_id + 1;
	uint64		size;
	uint32		nchunks;
	Manifest	nm;
	bool		full;
	int			max = 4;

	nm = *m;
	nm.nfiles = 0;
	nm.files = palloc(sizeof(ManifestFile) * (m->nfiles + max));
	for (int i = 0; i < m->nfiles; i++)
	{
		if (keep[i])
			nm.files[nm.nfiles++] = m->files[i];
	}
	/* (with a limit, as many files as it takes, each begun as the last fills) */
	do
	{
		if (!write_file(s, id, next, arg, stored, from, limit, xb, &full,
						&nchunks, &size))
			break;
		if (copies)
			pgstat_count_prolly(PGSTAT_PROLLY_CONJOIN_BYTES_COPIED, size);
		INJECTION_POINT("prolly-table-file-written", NULL);
		if (nm.nfiles == m->nfiles + max)
		{
			max *= 2;
			nm.files = repalloc(nm.files, sizeof(ManifestFile) * (m->nfiles + max));
		}
		nm.files[nm.nfiles].id = id;
		nm.files[nm.nfiles].nchunks = nchunks;
		nm.files[nm.nfiles].size = size;
		nm.files[nm.nfiles].flags = flags;
		nm.files[nm.nfiles].epoch_lo = epoch_lo;
		nm.files[nm.nfiles].epoch_hi = epoch_hi;
		nm.files[nm.nfiles].garbage = 0;
		nm.nfiles++;
		nm.last_id = id++;
	} while (full);
	if (nm.last_id == m->last_id)
	{
		pfree(nm.files);
		return;
	}
	/* (the committed root: one the transaction moves to may roll back) */
	scan_journal(s);
	nm.root = s->root;
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
 * The table files (ids) and journal segments (segs) of dir its manifest m
 * doesn't name, as kept or retiring; given (dropped): only those of ids m
 * gave, so none a writer is writing now (its id is past m's).
 */
static void
unnamed_files(const char *dir, const Manifest *m, bool dropped, List **ids,
			  List **segs)
{
	DIR		   *d = AllocateDir(dir);
	struct dirent *de;

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
		if (!named && (!dropped || id <= m->last_id))
		{
			uint64	   *p = palloc(sizeof(uint64));

			*p = id;
			if (segment)
				*segs = lappend(*segs, p);
			else
				*ids = lappend(*ids, p);
		}
	}
	FreeDir(d);
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
	List	   *ids = NIL;
	List	   *segs = NIL;
	ListCell   *lc;

	unnamed_files(dir, m, false, &ids, &segs);
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
	bool		made;

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
	ctl.hash = chunk_hash_key_hash;
	ctl.entrysize = sizeof(IndexEntry);
	s->ix = jindex_attach(dboid, spcoid, 2 * append_ranges(), &made);
	s->pending_index = hash_create("prolly journal pending", 256, &ctl,
								   HASH_ELEM | HASH_FUNCTION);
	initStringInfo(&s->pending);
	MemoryContextSwitchTo(old);

	/* the first since the server started: the index as last saved */
	if (made)
	{
		char	   *dir = GetDatabasePath(dboid, spcoid);
		char		format[FORMAT_SIZE];
		Manifest	m;

		uint64	   *sizes;

		(void) read_manifest(dir, &m);
		format_bytes(prolly_store_format(), format);
		/* (each segment's size now: what the index says past it, not yet) */
		sizes = palloc(sizeof(uint64) * Max(m.nsegments, 1));
		for (int i = 0; i < m.nsegments; i++)
		{
			char	   *path = segment_path(dboid, spcoid, m.segments[i]);
			struct stat st;

			sizes[i] = stat(path, &st) == 0 ? (uint64) st.st_size : 0;
			pfree(path);
		}
		(void) jindex_load(s->ix, format, FORMAT_SIZE, m.segments, sizes,
						   m.nsegments, JOURNAL_HEADER_SIZE);
		pfree(sizes);
		pfree(dir);
	}
	(void) ensure_journal(s, false);

	RegisterXactCallback(journal_xact_callback, NULL);
	RegisterSubXactCallback(journal_subxact_callback, NULL);
	session_store = s;
	return s;
}

static void transient_put(ProllyChunkStore *s, const ProllyHash *hash,
						  const void *data, Size len);

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
	{
		transient_put(s, hash, data, len);
		return;
	}
	if (len > MaxAllocSize - 64)
		elog(ERROR, "chunk of %zu bytes is too large", len);
	if (s->at_commit)
		elog(ERROR, "cannot put a chunk after the transaction's chunks are appended");
	xact_note(s);

	/* (whether the store has it is asked of all pending at once, later) */
	hash_key(&key, hash);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL)
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
	e->raw_len = (uint32) len;

	if (s->pending.len >= APPEND_BATCH)
		flush_pending(s);
}

/* A chunk a transient put kept: the transaction's until it ends. */
typedef struct TransientChunk
{
	ProllyHash	key;
	char	   *data;
	Size		len;
} TransientChunk;

static TransientChunk *
transient_find(ProllyChunkStore *s, const ProllyHash *key)
{
	if (s->transient == NULL)
		return NULL;
	return hash_search(s->transient, key, HASH_FIND, NULL);
}

/*
 * During recovery, what a read makes (a fold's tree on a hot standby) can't
 * be appended: it is kept in the transaction's memory instead, read and
 * found as the store's until the transaction ends, never written.
 */
static void
transient_put(ProllyChunkStore *s, const ProllyHash *hash, const void *data,
			  Size len)
{
	ProllyHash	key;
	TransientChunk *t;
	bool		found;
	MemoryContext old;

	hash_key(&key, hash);
	if (transient_find(s, &key) != NULL || stored_has(s, &key))
		return;
	old = MemoryContextSwitchTo(TopTransactionContext);
	if (s->transient == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.hash = chunk_hash_key_hash;
		ctl.entrysize = sizeof(TransientChunk);
		ctl.hcxt = TopTransactionContext;
		s->transient = hash_create("prolly transient chunks", 256, &ctl,
								   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}
	t = hash_search(s->transient, &key, HASH_ENTER, &found);
	t->data = palloc(Max(len, 1));
	memcpy(t->data, data, len);
	t->len = len;
	MemoryContextSwitchTo(old);
}

/*
 * The chunk's bytes from its record (rec, len bytes, in path) to out: a
 * chunk's record (checked once), and, if expected isn't NULL, that one's.
 */
static void
chunk_bytes_of(const char *path, const char *rec, uint32 len,
			   const ProllyHash *expected, StringInfo out)
{
	ProllyHash	h;
	Size		after;
	const char *chunk = NULL;
	Size		chunk_len;
	uint8		kind = len > sizeof(uint32) ? (uint8) rec[sizeof(uint32)] : 0;

	if ((kind != KIND_CHUNK && kind != KIND_XCHUNK) ||
		!record_valid(rec, len) || !record_hash(rec, len, &h, &after))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", path)));
	if (expected != NULL &&
		(h.len != expected->len || memcmp(h.bytes, expected->bytes, h.len) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk journal \"%s\" holds no chunk %s where its index says",
						path, prolly_hash_hex(expected))));
	/* (decompressed straight into out, if it holds nothing yet) */
	chunk = record_chunk(rec, len, after,
						 out->len == 0 ? out : chunk_scratch(), &chunk_len);
	if (chunk == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", path)));
	if (chunk != out->data)
		appendBinaryStringInfo(out, chunk, chunk_len);
}

/* The chunk's bytes from its record (rec, len bytes, in path) to out. */
static void
chunk_bytes(const char *path, const char *rec, uint32 len, StringInfo out)
{
	chunk_bytes_of(path, rec, len, NULL, out);
}

/*
 * The chunk a CHUNK or XCHUNK record holds, framed as stored chunks are,
 * without decoding it, appended to out (a journal record holds its codec
 * and, compressed, its length, as a frame does), its hash to *hash; false,
 * appending nothing, if the record isn't one, whole and valid.
 */
static bool
record_frame(const char *rec, uint32 len, ProllyHash *hash, StringInfo out)
{
	Size		after;
	const char *p;
	const char *end = rec + len - sizeof(uint32);
	char		head[PROLLY_FRAME_HEADER];
	uint32		chunk_len;
	uint8		kind = len > sizeof(uint32) ? (uint8) rec[sizeof(uint32)] : 0;

	/* (a chunk's record, as chunk_bytes_of() asks: not a ROOT's, nor a PAD's) */
	if ((kind != KIND_CHUNK && kind != KIND_XCHUNK) ||
		!record_valid(rec, len) || !record_hash(rec, len, hash, &after) ||
		(p = rec + after) >= end)
		return false;
	if ((uint8) *p == CODEC_NONE)
	{
		chunk_len = (uint32) (end - (p + 1));
		p += 1;
	}
	else if ((uint8) *p == CODEC_LZ4 && end - p >= (ptrdiff_t) CODEC_HEAD_MAX)
	{
		memcpy(&chunk_len, p + 1, sizeof(chunk_len));
		p += CODEC_HEAD_MAX;
	}
	else
		return false;
	head[0] = (char) (*(rec + after) == CODEC_NONE ? PROLLY_CODEC_NONE :
					  PROLLY_CODEC_LZ4);
	head[1] = chunk_len & 0xff;
	head[2] = (chunk_len >> 8) & 0xff;
	head[3] = (chunk_len >> 16) & 0xff;
	head[4] = (chunk_len >> 24) & 0xff;
	appendBinaryStringInfo(out, head, PROLLY_FRAME_HEADER);
	appendBinaryStringInfo(out, p, end - p);
	return true;
}

/* record_frame() of a record of path, corrupt if it isn't a chunk's. */
static void
record_framed(const char *path, const char *rec, uint32 len, StringInfo out)
{
	ProllyHash	h;

	if (!record_frame(rec, len, &h, out))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", path)));
}

static bool journal_get(ProllyChunkStore *s, const ProllyHash *hash,
						StringInfo out);
static bool journal_read(ProllyChunkStore *s, const ProllyHash *hash,
						 StringInfo out, bool stored);

/* a chunk read ahead */
typedef struct AheadEntry
{
	ProllyHash	hash;			/* key */
	char	   *data;
	Size		len;
} AheadEntry;

/*
 * what the store reads ahead at most: past it, no more is kept until some
 * is taken
 */
#define AHEAD_MAX			(8 * 1024 * 1024)

typedef struct AheadFill
{
	ProllyChunkStore *s;
	const ProllyHash *hashes;
} AheadFill;

static void
ahead_put(int i, const char *data, Size len, void *arg)
{
	AheadFill  *af = arg;
	ProllyChunkStore *s = af->s;
	AheadEntry *e;
	bool		found;

	if (data == NULL)
		return;
	/* (full: what is there is kept, for what asks next; this isn't) */
	if (s->ahead != NULL && s->ahead_bytes + len > AHEAD_MAX)
		return;
	if (s->ahead == NULL)
	{
		HASHCTL		ctl;

		if (s->ahead_cxt == NULL)
			s->ahead_cxt = AllocSetContextCreate(TopMemoryContext,
												 "prolly journal read-ahead",
												 ALLOCSET_DEFAULT_SIZES);
		else
			MemoryContextReset(s->ahead_cxt);
		ctl.keysize = sizeof(ProllyHash);
		ctl.hash = chunk_hash_key_hash;
		ctl.entrysize = sizeof(AheadEntry);
		ctl.hcxt = s->ahead_cxt;
		s->ahead = hash_create("prolly journal read-ahead", 256, &ctl,
							   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
		s->ahead_bytes = 0;
	}
	e = hash_search(s->ahead, &af->hashes[i], HASH_ENTER, &found);
	if (found)
		return;
	e->data = MemoryContextAlloc(s->ahead_cxt, Max(len, 1));
	memcpy(e->data, data, len);
	e->len = len;
	s->ahead_bytes += len;
}

/* A chunk read ahead, given out (once) to out. */
static bool
ahead_take(ProllyChunkStore *s, const ProllyHash *key, StringInfo out)
{
	AheadEntry *e;

	if (s->ahead == NULL ||
		(e = hash_search(s->ahead, key, HASH_FIND, NULL)) == NULL)
		return false;
	appendBinaryStringInfo(out, e->data, e->len);
	s->ahead_bytes -= e->len;
	pfree(e->data);
	hash_search(s->ahead, key, HASH_REMOVE, NULL);
	return true;
}

/* Forget what was read ahead (the transaction ended). */
static void
ahead_forget(ProllyChunkStore *s)
{
	if (s->ahead_cxt != NULL)
		MemoryContextReset(s->ahead_cxt);
	s->ahead = NULL;
	s->ahead_bytes = 0;
}

/* a chunk get_many() reads, from a segment or a table file */
typedef struct ManyRead
{
	int			i;				/* in the request */
	Segment    *g;				/* the segment's, or NULL */
	TableFile  *t;				/* or the table file's */
	uint64		offset;
	uint32		len;
} ManyRead;

/* reads at most this far apart are made as one, up to MANY_MAX */
#define MANY_GAP			4096
#define MANY_MAX			(1024 * 1024)

static int
many_cmp(const void *a, const void *b)
{
	const ManyRead *x = a;
	const ManyRead *y = b;
	uintptr_t	fx = x->g ? (uintptr_t) x->g : (uintptr_t) x->t;
	uintptr_t	fy = y->g ? (uintptr_t) y->g : (uintptr_t) y->t;

	if (fx != fy)
		return fx < fy ? -1 : 1;
	if (x->offset != y->offset)
		return x->offset < y->offset ? -1 : 1;
	return 0;
}

/* Where key is in the journal or a table file: false if neither has it. */
static bool
many_locate(ProllyChunkStore *s, const ProllyHash *key, ManyRead *r)
{
	uint64		segment;
	ListCell   *lc;

	if (jindex_find(s->ix, key, true, &segment, &r->offset, &r->len))
	{
		r->g = local_segment(s, segment);
		r->t = NULL;
		/* (as get() takes it: the index names it, its file must be there) */
		if (!open_segment(r->g))
			elog(ERROR, "chunk journal segment %llu is missing",
				 (unsigned long long) segment);
		return true;
	}
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);

		if (PROLLY_CALL(table_format(), find, t->tf, key, &r->offset, &r->len))
		{
			r->g = NULL;
			r->t = t;
			return true;
		}
	}
	return false;
}

/*
 * get_many(): every chunk found first (the journal read, and the table
 * files' list, once for all that miss), then read file by file in offset
 * order, reads close together made as one, each announced to the kernel
 * before any is made.  A table file a conjoin removed meanwhile is read
 * from again by get(), which looks again.
 *
 * The reads point at the store's segments and table files, which a
 * refresh (reading the journal or the files' list again) may free: so
 * every chunk is located after the last refresh (all again, if some
 * missed the first time), and once get() has been asked for one (which
 * may refresh), it is asked for the rest too.
 */
static void
journal_get_many(ProllyChunkStore *s, const ProllyHash *hashes, int n,
				 ProllyChunkManyVisit visit, void *arg)
{
	ManyRead   *reads = palloc(sizeof(ManyRead) * Max(n, 1));
	int			nreads = 0;
	int		   *missing = palloc(sizeof(int) * Max(n, 1));
	int			nmissing = 0;
	int		   *rest = palloc(sizeof(int) * Max(n, 1));
	int			nrest = 0;
	bool		asked = false;
	StringInfoData buf;
	StringInfoData out;

	initStringInfo(&out);
	refresh_segments(s);
	xact_note(s);
	for (int i = 0; i < n; i++)
	{
		ProllyHash	key;
		IndexEntry *e;

		hash_key(&key, &hashes[i]);
		e = hash_search(s->pending_index, &key, HASH_FIND, NULL);
		resetStringInfo(&out);
		if (e != NULL)
		{
			chunk_bytes("(pending)", s->pending.data + e->offset, e->len, &out);
			visit(i, out.data, out.len, arg);
		}
		else if (transient_find(s, &key) != NULL)
		{
			TransientChunk *t = transient_find(s, &key);

			visit(i, t->data, t->len, arg);
		}
		else if (ahead_take(s, &key, &out))
			visit(i, out.data, out.len, arg);
		else
			rest[nrest++] = i;
	}
	for (bool refreshed = false;;)
	{
		nreads = nmissing = 0;
		for (int r = 0; r < nrest; r++)
		{
			ProllyHash	key;

			hash_key(&key, &hashes[rest[r]]);
			if (many_locate(s, &key, &reads[nreads]))
				reads[nreads++].i = rest[r];
			else
				missing[nmissing++] = rest[r];
		}
		if (nmissing == 0 || refreshed)
			break;
		INJECTION_POINT("prolly-get-many-refreshing", NULL);
		scan_journal(s);
		load_tables(s, false);
		refreshed = true;
	}

	qsort(reads, nreads, sizeof(ManyRead), many_cmp);
	/* the runs of reads made as one, announced first */
	for (int a = 0; a < nreads;)
	{
		int			b = a + 1;
		uint64		end = reads[a].offset + reads[a].len;

		while (b < nreads && reads[b].g == reads[a].g && reads[b].t == reads[a].t &&
			   reads[b].offset <= end + MANY_GAP &&
			   reads[b].offset + reads[b].len - reads[a].offset <= MANY_MAX)
		{
			end = Max(end, reads[b].offset + reads[b].len);
			b++;
		}
		(void) FilePrefetch(reads[a].g ? reads[a].g->file : reads[a].t->file,
							reads[a].offset, end - reads[a].offset,
							WAIT_EVENT_DATA_FILE_PREFETCH);
		a = b;
	}
	initStringInfo(&buf);
	for (int a = 0; a < nreads;)
	{
		int			b = a + 1;
		uint64		start = reads[a].offset;
		uint64		end = start + reads[a].len;
		File		file = reads[a].g ? reads[a].g->file : reads[a].t->file;
		ssize_t		got;

		while (b < nreads && reads[b].g == reads[a].g && reads[b].t == reads[a].t &&
			   reads[b].offset <= end + MANY_GAP &&
			   reads[b].offset + reads[b].len - start <= MANY_MAX)
		{
			end = Max(end, reads[b].offset + reads[b].len);
			b++;
		}
		CHECK_FOR_INTERRUPTS();
		resetStringInfo(&buf);
		got = -1;
		if (!asked)
		{
			enlargeStringInfo(&buf, (int) (end - start));
			got = FileRead(file, buf.data, end - start, start,
						   WAIT_EVENT_DATA_FILE_READ);
			pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_FILE_READS, 1);
		}
		for (int r = a; r < b; r++)
		{
			ManyRead   *mr = &reads[r];
			const char *bytes = buf.data + (mr->offset - start);

			resetStringInfo(&out);
			if (asked || got != (ssize_t) (end - start))
			{
				/*
				 * (a file gone, or short: asked for alone, and from here on
				 * every one, as get() may have freed what reads[] names)
				 */
				asked = true;
				if (!journal_get(s, &hashes[mr->i], &out))
				{
					visit(mr->i, NULL, 0, arg);
					continue;
				}
			}
			else if (mr->g != NULL)
				chunk_bytes_of(mr->g->path, bytes, mr->len, &hashes[mr->i], &out);
			else
				PROLLY_CALL(table_format(), decode, mr->t->tf, bytes, mr->len, &out);
			visit(mr->i, out.data, out.len, arg);
		}
		a = b;
	}
	for (int m = 0; m < nmissing; m++)
	{
		resetStringInfo(&out);
		if (s->through != NULL && s->through(s->through_arg, &hashes[missing[m]], &out))
			visit(missing[m], out.data, out.len, arg);
		else
			visit(missing[m], NULL, 0, arg);
	}
	pfree(buf.data);
	pfree(out.data);
	pfree(reads);
	pfree(missing);
	pfree(rest);
}

static bool
journal_get(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	return journal_read(s, hash, out, false);
}

/*
 * A chunk as stored, framed, without decoding it: a record's or a table
 * file's bytes (one only read ahead is read again; one read through a
 * source is framed).
 */
static bool
journal_get_stored(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	return journal_read(s, hash, out, true);
}

static bool
journal_read(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out,
			 bool stored)
{
	ProllyHash	key;
	IndexEntry *e;
	Segment    *g;
	char	   *rec;
	uint64		segment;
	uint64		offset;
	uint32		len;
	int			start;

	hash_key(&key, hash);
	e = hash_search(s->pending_index, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		if (stored)
			record_framed("(pending)", s->pending.data + e->offset, e->len, out);
		else
			chunk_bytes("(pending)", s->pending.data + e->offset, e->len, out);
		return true;
	}
	{
		TransientChunk *t = transient_find(s, &key);

		if (t != NULL)
		{
			if (stored)
				prolly_chunk_frame(t->data, t->len, out);
			else
				appendBinaryStringInfo(out, t->data, t->len);
			return true;
		}
	}
	if (!stored && ahead_take(s, &key, out))
		return true;
	/* (the store's generation as of now: a collection since lets go of all) */
	if (!stored && s == session_store)
	{
		refresh_segments(s);
		start = out->len;
		if (chunk_cache_take(s, &key, out, s->gc_generation))
		{
			xact_note(s);
			trust_named(s, out->data + start, out->len - start,
						s->gc_generation);
			return true;
		}
	}
	if (stored)
	{
		StringInfoData ahead;

		/* (read ahead, decoded: framed again rather than read again) */
		initStringInfo(&ahead);
		if (ahead_take(s, &key, &ahead))
		{
			prolly_chunk_frame(ahead.data, ahead.len, out);
			pfree(ahead.data);
			return true;
		}
		pfree(ahead.data);
	}
	refresh_segments(s);
	xact_note(s);
	start = out->len;
	if (!jindex_find(s->ix, &key, true, &segment, &offset, &len))
	{
		if (tables_read(s, &key, out, stored))
		{
			if (!stored && s == session_store)
			{
				chunk_cache_put(s, &key, out->data + start, out->len - start,
								s->gc_generation, false);
				trust_named(s, out->data + start, out->len - start,
							s->gc_generation);
			}
			return true;
		}
		scan_journal(s);
		if (!jindex_find(s->ix, &key, true, &segment, &offset, &len))
		{
			load_tables(s, false);
			if (tables_read(s, &key, out, stored))
			{
				if (!stored && s == session_store)
				{
					chunk_cache_put(s, &key, out->data + start, out->len - start,
									s->gc_generation, false);
					trust_named(s, out->data + start, out->len - start,
								s->gc_generation);
				}
				return true;
			}
			if (s->through == NULL)
				return false;
			if (!stored)
				return s->through(s->through_arg, hash, out);
			{
				StringInfoData data;
				bool		found;

				initStringInfo(&data);
				found = s->through(s->through_arg, hash, &data);
				if (found)
					prolly_chunk_frame(data.data, data.len, out);
				pfree(data.data);
				return found;
			}
		}
	}
	g = local_segment(s, segment);
	if (!open_segment(g))
		elog(ERROR, "chunk journal segment %llu is missing",
			 (unsigned long long) segment);
	rec = palloc(len);
	read_exact(g, rec, len, offset);
	pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_FILE_READS, 1);
	/* (the index named it: its record must hold it) */
	if (stored)
	{
		ProllyHash	h;
		Size		after;

		if (!record_valid(rec, len) || !record_hash(rec, len, &h, &after) ||
			h.len != key.len || memcmp(h.bytes, key.bytes, key.len) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("chunk journal \"%s\" holds no chunk %s at %llu",
							g->path, prolly_hash_hex(hash),
							(unsigned long long) offset)));
		record_framed(g->path, rec, len, out);
	}
	else
	{
		chunk_bytes_of(g->path, rec, len, &key, out);
		if (s == session_store)
		{
			chunk_cache_put(s, &key, out->data + start, out->len - start,
							s->gc_generation, false);
			trust_named(s, out->data + start, out->len - start,
						s->gc_generation);
		}
	}
	pfree(rec);
	return true;
}

static bool
journal_has(ProllyChunkStore *s, const ProllyHash *hash)
{
	ProllyHash	key;

	hash_key(&key, hash);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL ||
		transient_find(s, &key) != NULL)
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

/* The lock journal_move_root() takes, if free; false if another holds it. */
static bool
journal_try_hold_root(ProllyChunkStore *s)
{
	LOCKTAG		tag;

	if (s->moving)
		return true;
	SET_LOCKTAG_OBJECT(tag, s->dboid, ROOT_LOCK_CLASSID, 0, 0);
	return LockAcquire(&tag, ExclusiveLock, false, true) != LOCKACQUIRE_NOT_AVAIL;
}

/* Is journal_move_root()'s lock free now?  (Taken and let go of at once.) */
static bool
journal_root_free(ProllyChunkStore *s)
{
	LOCKTAG		tag;

	if (s->moving)
		return true;
	SET_LOCKTAG_OBJECT(tag, s->dboid, ROOT_LOCK_CLASSID, 0, 0);
	if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		return false;
	LockRelease(&tag, ExclusiveLock, false);
	return true;
}

/*
 * Wait for journal_move_root()'s lock to be free, in its queue, taking
 * nothing (taken and let go of at once).
 */
static void
journal_wait_root_free(ProllyChunkStore *s)
{
	LOCKTAG		tag;

	if (s->moving)
		return;
	SET_LOCKTAG_OBJECT(tag, s->dboid, ROOT_LOCK_CLASSID, 0, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
	LockRelease(&tag, ExclusiveLock, false);
}

/* The GC generation of the manifest now (read again if it changed). */
static uint32
journal_generation(ProllyChunkStore *s)
{
	refresh_segments(s);
	return s->gc_generation;
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
	if (s->at_commit)
		pending_cached(s);
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
	request_sync(p.dboid, p.spcoid, SYNC_SEGMENT, p.segment, sync_refused());
	pfree(path);
	pfree(rec.data);
}

/* ----------------------------------------------------------------
 * Copies in redo (#564)
 *
 * A copy record names where its bytes come from, in the server's own
 * files: redo reads them there, checks their CRC and writes them.  So a
 * copy's sources must hold, when it is replayed, the bytes they held when
 * it was logged:
 *
 * - A table file's bytes are written once, by records before any copy
 *   from it (a copy's sources are named in the manifest, so were synced
 *   and named first), and never again: ids aren't given twice.  A
 *   segment's records are written by appends before the copy, and a
 *   segment is only appended to.  So redo running forward finds them, or
 *   finds the file gone.
 * - A file is gone only once a later record removed it (an unlink, a
 *   retire, its database dropped): crash recovery or a standby's restart
 *   replays from before that.  Then the copy's own file was synced before
 *   the source went: a writer syncs its file before the manifest naming
 *   it (and conjoin, collections and CREATE DATABASE remove their sources
 *   only after), and redo syncs the files its copies wrote before it
 *   replays a removal.  So the file already holds the copy's bytes, which
 *   redo checks against the copy's CRC, and leaves.
 * - An online backup reads the files while they change.  None is removed
 *   while a backup runs (remove_store_file(): on a standby too, its own
 *   backups), so a source a backup's redo copies from is in it, or was
 *   removed before the backup began, the copy's file synced before then,
 *   and in it whole.  CREATE DATABASE's template may be dropped while one
 *   runs: its copies log the bytes while a backup runs, as full pages are
 *   while one does.  A backup taken from a standby can't tell its primary
 *   so; there a template dropped during the backup, after a CREATE
 *   DATABASE from it that the backup's redo replays, fails the restore at
 *   the consistent point, below, not silently.
 * - A copy whose sources fail it and whose file doesn't hold its bytes
 *   waits for a later record to remove that file (the file was copied
 *   and removed, both after the point redo began from); it may only before
 *   recovery is consistent (a removal's redo moves the minimum recovery
 *   point past it), and one still waiting then is a PANIC, as is a copy
 *   that fails once consistent: the server's files differ from those the
 *   WAL was written against.
 * ----------------------------------------------------------------
 */

/* a store file, by its database and id */
typedef struct StoreFile
{
	Oid			dboid;
	Oid			spcoid;
	bool		segment;
	uint64		id;
	XLogRecPtr	lsn;			/* (a pending copy's) */
	char	   *why;			/* (a pending copy's: its sources failed) */
} StoreFile;

/*
 * Redo's (the startup process's): the files copies wrote, not synced yet;
 * those to remove once the backups running end; the copies waiting for
 * their files to be removed.
 */
static List *copied_unsynced = NIL;
static List *deferred_unlinks = NIL;
static List *copies_pending = NIL;
static MemoryContext copy_redo_cxt = NULL;

static char *
store_file_path(const StoreFile *f)
{
	char	   *dir;
	char	   *path;

	if (f->segment)
		return segment_path(f->dboid, f->spcoid, f->id);
	dir = GetDatabasePath(f->dboid, f->spcoid);
	path = table_file_path(dir, f->id);
	pfree(dir);
	return path;
}

static void
note_file(List **files, Oid dboid, Oid spcoid, bool segment, uint64 id)
{
	MemoryContext old;
	StoreFile  *f;

	foreach_ptr(StoreFile, g, *files)
	{
		if (g->dboid == dboid && g->spcoid == spcoid &&
			g->segment == segment && g->id == id)
			return;
	}
	old = MemoryContextSwitchTo(TopMemoryContext);
	f = palloc0(sizeof(StoreFile));
	f->dboid = dboid;
	f->spcoid = spcoid;
	f->segment = segment;
	f->id = id;
	*files = lappend(*files, f);
	MemoryContextSwitchTo(old);
}

/* Forget the files of dboid (of id, unless all) a list has. */
static void
forget_files(List **files, Oid dboid, Oid spcoid, bool all, bool segment,
			 uint64 id)
{
	foreach_ptr(StoreFile, f, *files)
	{
		if (f->dboid != dboid || (!all && (f->spcoid != spcoid ||
										   f->segment != segment || f->id != id)))
			continue;
		if (f->why)
			pfree(f->why);
		*files = foreach_delete_current(*files, f);
		pfree(f);
	}
}

/* Sync the files copies wrote: a source of theirs is about to be removed. */
static void
copied_sync(void)
{
	foreach_ptr(StoreFile, f, copied_unsynced)
	{
		char	   *path = store_file_path(f);

		sync_if_exists(path);
		pfree(path);
	}
	list_free_deep(copied_unsynced);
	copied_unsynced = NIL;
}

static void
defer_unlink(Oid dboid, Oid spcoid, bool segment, uint64 id)
{
	note_file(&deferred_unlinks, dboid, spcoid, segment, id);
}

/* Remove the files whose removal waited for backups, once none runs. */
static void
deferred_run(void)
{
	if (deferred_unlinks == NIL || XLogBackupInProgress())
		return;
	foreach_ptr(StoreFile, f, deferred_unlinks)
	{
		char	   *path = store_file_path(f);

		if (unlink(path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		pfree(path);
	}
	list_free_deep(deferred_unlinks);
	deferred_unlinks = NIL;
	manifests_changed();
}

/*
 * A file a store file's removal waits for, or a copy into it waits on,
 * removed by redo (all: its database dropped).
 */
static void
redo_removed(Oid dboid, Oid spcoid, bool all, bool segment, uint64 id)
{
	forget_files(&copies_pending, dboid, spcoid, all, segment, id);
	if (all)
		forget_files(&deferred_unlinks, dboid, spcoid, all, segment, id);
}

/*
 * A table file's first bytes in redo: they cut short what a failed writer
 * left, but not a file the manifest names already (replayed again, after
 * a crash or a standby's restart): readers may have it mapped, and a
 * mapping cut short is a SIGBUS.  Its bytes are the WAL's, so written over
 * in place they are the same.  A removal of its id waiting for backups is
 * forgotten: the file is written anew.
 */
static void
redo_table_file_begun(Oid dboid, Oid spcoid, const char *dir, uint64 id,
					  const char *path)
{
	Manifest	m;
	bool		named = false;

	forget_files(&deferred_unlinks, dboid, spcoid, false, false, id);
	if (read_manifest(dir, &m))
	{
		for (int i = 0; i < m.nfiles; i++)
			named |= m.files[i].id == id;
	}
	if (!named)
	{
		int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);

		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		CloseTransientFile(fd);
	}
}

/*
 * The bytes a copy's sources (n of them, at sources, of the database
 * src_dboid) make, appended to out; NULL, or why they can't be had.
 */
static char *
copy_sources(Oid src_dboid, Oid src_spcoid, const char *sources, uint32 n,
			 StringInfo out)
{
	const ProllyTableFileRoutine *fmt = table_format();
	ProllyTableFileWriter *w = NULL;
	char	   *dir = GetDatabasePath(src_dboid, src_spcoid);
	StringInfoData rec;
	StringInfoData frame;
	char	   *path = NULL;
	char	   *why = NULL;
	int			fd = -1;

	initStringInfo(&rec);
	initStringInfo(&frame);
	for (uint32 i = 0; i < n && why == NULL; i++)
	{
		CopySource src;
		ProllyHash	h;
		const char *p = sources + (Size) i * COPY_SOURCE_SIZE;
		char	   *spath;

		src.kind = (uint8) p[0];
		memcpy(&src.id, p + 1, sizeof(uint64));
		memcpy(&src.offset, p + 9, sizeof(uint64));
		memcpy(&src.len, p + 17, sizeof(uint32));
		if (src.kind < COPY_TABLE_BYTES || src.kind > COPY_SEGMENT_CHUNK)
			elog(PANIC, "chunk store copy source of kind %u", src.kind);
		spath = src.kind == COPY_TABLE_BYTES ? table_file_path(dir, src.id) :
			segment_path(src_dboid, src_spcoid, src.id);
		if (path == NULL || strcmp(path, spath) != 0)
		{
			if (fd >= 0)
				CloseTransientFile(fd);
			if (path)
				pfree(path);
			path = spath;
			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
			if (fd < 0)
			{
				why = psprintf("could not open file \"%s\": %m", path);
				break;
			}
		}
		else
			pfree(spath);
		if (src.kind != COPY_SEGMENT_CHUNK)
		{
			enlargeStringInfo(out, src.len);
			if (pg_pread(fd, out->data + out->len, src.len, src.offset) != src.len)
				why = psprintf("file \"%s\" has no %u bytes at %llu", path,
							   src.len, (unsigned long long) src.offset);
			else
				out->len += src.len;
			continue;
		}
		resetStringInfo(&rec);
		resetStringInfo(&frame);
		enlargeStringInfo(&rec, src.len);
		if (pg_pread(fd, rec.data, src.len, src.offset) != src.len ||
			!record_frame(rec.data, src.len, &h, &frame))
		{
			why = psprintf("file \"%s\" has no chunk record of %u bytes at %llu",
						   path, src.len, (unsigned long long) src.offset);
			continue;
		}
		/* (as the writer added it: the format's bytes for the frame) */
		if (w == NULL)
			w = PROLLY_CALL(fmt, write_begin, CurrentMemoryContext);
		prolly_tablefile_add_stored(fmt, w, &h, frame.data, frame.len, out);
	}
	if (fd >= 0)
		CloseTransientFile(fd);
	return why;
}

/* Does path hold len bytes at offset whose CRC-32C is crc? */
static bool
file_holds(const char *path, uint64 offset, uint32 len, pg_crc32c crc)
{
	int			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	char	   *bytes;
	pg_crc32c	got;
	bool		holds;

	if (fd < 0)
		return false;
	bytes = palloc(Max(len, 1));
	holds = pg_pread(fd, bytes, len, offset) == len;
	CloseTransientFile(fd);
	if (holds)
	{
		INIT_CRC32C(got);
		COMP_CRC32C(got, bytes, len);
		FIN_CRC32C(got);
		holds = EQ_CRC32C(got, crc);
	}
	pfree(bytes);
	return holds;
}

static void
copy_redo(XLogReaderState *record)
{
	xl_chunks_copy xlrec;
	const char *sources = XLogRecGetData(record) + SizeOfChunksCopy;
	MemoryContext old;
	StringInfoData out;
	char	   *dir;
	char	   *path;
	char	   *why;
	struct stat st;

	memcpy(&xlrec, XLogRecGetData(record), SizeOfChunksCopy);
	if (XLogRecGetDataLen(record) !=
		SizeOfChunksCopy + (Size) xlrec.nsources * COPY_SOURCE_SIZE)
		elog(PANIC, "chunk store copy record of %u bytes",
			 XLogRecGetDataLen(record));
	if (copy_redo_cxt == NULL)
		copy_redo_cxt = AllocSetContextCreate(TopMemoryContext,
											  "prolly copy redo",
											  ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(copy_redo_cxt);
	dir = GetDatabasePath(xlrec.dboid, xlrec.spcoid);
	path = xlrec.segment ? segment_path(xlrec.dboid, xlrec.spcoid, xlrec.id) :
		table_file_path(dir, xlrec.id);
	initStringInfo(&out);
	why = copy_sources(xlrec.src_dboid, xlrec.src_spcoid, sources,
					   xlrec.nsources, &out);
	if (why == NULL)
	{
		pg_crc32c	crc;

		INIT_CRC32C(crc);
		COMP_CRC32C(crc, out.data, out.len);
		FIN_CRC32C(crc);
		if (out.len != xlrec.len || !EQ_CRC32C(crc, xlrec.crc))
			why = psprintf("the %d bytes its %u sources in database directory \"%s\" make don't match the %u bytes copied (CRC %08X, not %08X)",
						   out.len, xlrec.nsources,
						   GetDatabasePath(xlrec.src_dboid, xlrec.src_spcoid),
						   xlrec.len, crc, xlrec.crc);
	}
	if (why == NULL && stat(dir, &st) != 0)
		why = psprintf("database directory \"%s\" is gone", dir);
	if (why == NULL)
	{
		if (xlrec.offset == 0 && !xlrec.segment)
			redo_table_file_begun(xlrec.dboid, xlrec.spcoid, dir, xlrec.id, path);
		write_at(path, out.data, out.len, xlrec.offset);
		request_sync(xlrec.dboid, xlrec.spcoid,
					 xlrec.segment ? SYNC_SEGMENT : SYNC_TABLE_FILE, xlrec.id,
					 sync_refused());
		note_file(&copied_unsynced, xlrec.dboid, xlrec.spcoid, xlrec.segment,
				  xlrec.id);
	}
	else if (!file_holds(path, xlrec.offset, xlrec.len, xlrec.crc))
	{
		StoreFile  *f;

		if (reachedConsistency)
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not replay a copy into chunk store file \"%s\"",
							path),
					 errdetail_internal("%s", why)));
		note_file(&copies_pending, xlrec.dboid, xlrec.spcoid, xlrec.segment,
				  xlrec.id);
		f = llast(copies_pending);
		if (f->why == NULL)
		{
			f->lsn = record->ReadRecPtr;
			f->why = MemoryContextStrdup(TopMemoryContext, why);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(copy_redo_cxt);
}

/*
 * The files of database directory dir (of dboid, in spcoid) its manifest
 * dropped, as archive recovery reaches a consistent state with no backup
 * running: those whose removal waited for a backup that ran as their
 * retire was replayed (a standby's own), and that a restart since forgot,
 * the retire's redo being before the point redo began from.  The primary
 * removed them long ago and logs no other removal; a standby runs no
 * writer whose remove_unnamed() would take them.  Only the ids the
 * manifest gave: a file past them may be one whose writer's records redo
 * is still replaying.  Not WAL-logged: a standby can't, and its own
 * standbys do the same.
 */
static void
remove_dropped(const char *dir, Oid dboid, Oid spcoid)
{
	Manifest	m;
	List	   *ids = NIL;
	List	   *segs = NIL;
	ListCell   *lc;
	bool		found;

	PG_TRY();
	{
		found = read_manifest(dir, &m);
	}
	PG_CATCH();
	{
		/* (a store this server refuses is left as it is) */
		EmitErrorReport();
		FlushErrorState();
		return;
	}
	PG_END_TRY();
	if (!found)
		return;
	unnamed_files(dir, &m, true, &ids, &segs);
	foreach(lc, ids)
	{
		char	   *path = table_file_path(dir, *(uint64 *) lfirst(lc));

		if (unlink(path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		pfree(path);
	}
	foreach(lc, segs)
	{
		char	   *path = segment_path(dboid, spcoid, *(uint64 *) lfirst(lc));

		if (unlink(path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		pfree(path);
	}
	if (ids != NIL || segs != NIL)
		manifests_changed();
	list_free_deep(ids);
	list_free_deep(segs);
}

/*
 * Recovery is consistent: a copy still waiting for its file's removal
 * means the files differ from those the WAL was written against.  In
 * archive recovery, with no backup running, the files dropped whose
 * removal a restart forgot are removed (crash recovery's end removes what
 * the manifests don't name, WAL-logged, instead).
 */
static void
journal_redo_consistent(void)
{
	StoreFile  *f;
	char	   *path;

	if (ArchiveRecoveryRequested && !XLogBackupInProgress())
		each_database_dir(remove_dropped);
	if (copies_pending == NIL)
		return;
	f = linitial(copies_pending);
	path = store_file_path(f);
	ereport(PANIC,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk store file \"%s\" lacks the bytes copied to it at %X/%08X",
					path, LSN_FORMAT_ARGS(f->lsn)),
			 errdetail_internal("The copy's sources failed it: %s; and no later record removed the file.",
								f->why)));
}

/*
 * Replay a table file's bytes, a manifest (after syncing the files it
 * names, which replay wrote without syncing) or an unlink.  A removal moves
 * the minimum recovery point past it first, as a truncation's redo does:
 * redo after a restart doesn't count as consistent before it, while copies
 * from what it removes may still wait on it.
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
			redo_table_file_begun(xlrec.dboid, xlrec.spcoid, dir, xlrec.id, path);
		write_at(path, XLogRecGetData(record) + SizeOfChunksFile,
				 XLogRecGetDataLen(record) - SizeOfChunksFile, xlrec.offset);
		request_sync(xlrec.dboid, xlrec.spcoid, SYNC_TABLE_FILE, xlrec.id,
					 sync_refused());
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
		{
			swap_holders_resolve(xlrec.dboid, xlrec.spcoid);
			standby_readers_note(xlrec.dboid);
			/* (and a logical slot being created starts after it) */
			ReplicationSlotsCollectionReplayed(record->EndRecPtr);
		}
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
		if (info == XLOG_CHUNKS_SWAP)
			swap_replayed();
		if (info == XLOG_CHUNKS_RETIRE)
		{
			XLogFlush(record->EndRecPtr);
			copied_sync();
			for (int i = 0; i < before.nretiring; i++)
			{
				redo_removed(xlrec.dboid, xlrec.spcoid, false, false,
							 before.retiring[i]);
				redo_removed(xlrec.dboid, xlrec.spcoid, false, true,
							 before.retiring[i]);
			}
			retire_files(xlrec.dboid, xlrec.spcoid, &before, &m);
		}
	}
	else
	{
		bool		segment = xlrec.offset == UNLINK_SEGMENT;
		char	   *path = segment ?
			segment_path(xlrec.dboid, xlrec.spcoid, xlrec.id) :
			table_file_path(dir, xlrec.id);

		XLogFlush(record->EndRecPtr);
		copied_sync();
		redo_removed(xlrec.dboid, xlrec.spcoid, false, segment, xlrec.id);
		remove_store_file(xlrec.dboid, xlrec.spcoid, segment, xlrec.id, path);
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

	deferred_run();
	switch (info & PROLLY_XLOG_TYPE_MASK)
	{
		case XLOG_CHUNKS_APPEND:
			memcpy(&xlrec, XLogRecGetData(record), SizeOfChunksAppend);
			path = segment_path(xlrec.dboid, xlrec.spcoid, xlrec.segment);
			write_at(path, XLogRecGetData(record) + SizeOfChunksAppend,
					 XLogRecGetDataLen(record) - SizeOfChunksAppend, xlrec.offset);
			request_sync(xlrec.dboid, xlrec.spcoid, SYNC_SEGMENT, xlrec.segment,
						 sync_refused());
			pfree(path);
			break;
		case XLOG_CHUNKS_COPY:
			copy_redo(record);
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
		if (strncmp(de->d_name, "prolly_table_", 13) != 0 &&
			strncmp(de->d_name, RECEIVED_PREFIX, strlen(RECEIVED_PREFIX)) != 0)
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
 * Make every journal's appends durable: once the appends reserved before
 * the redo point are written, the checkpoint fsyncs the files their sync
 * requests name (what was written since the last one), after this.  A
 * request the queue refused meanwhile has every store's files fsynced
 * here instead.
 */
static void
journal_checkpoint(void)
{
	DIR		   *tsdir;
	struct dirent *de;

	/* the appends reserved before the redo point, written */
	wait_reserved(NULL);
	/* (the indexes are a cache, written to be read at the next start) */
	{
		char		format[FORMAT_SIZE];

		format_bytes(prolly_store_format(), format);
		jindex_checkpoint(format, FORMAT_SIZE);
	}
	if (pg_atomic_exchange_u32(&shared()->sync_all, 0) == 0)
		return;

	/* (a scan that fails leaves the next checkpoint to scan again) */
	PG_TRY();
	{
		sync_journals_under("base");
		tsdir = AllocateDir(PG_TBLSPC_DIR);
		while (tsdir != NULL && (de = ReadDir(tsdir, PG_TBLSPC_DIR)) != NULL)
		{
			char	   *sub;

			if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
				continue;
			sub = psprintf("%s/%s/%s", PG_TBLSPC_DIR, de->d_name,
						   TABLESPACE_VERSION_DIRECTORY);
			sync_journals_under(sub);
			pfree(sub);
		}
		if (tsdir != NULL)
			FreeDir(tsdir);
	}
	PG_CATCH();
	{
		pg_atomic_write_u32(&shared()->sync_all, 1);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * A database dropped: its shared index, its segments' checked ends and its
 * sync requests forgotten, so a database given its OID later starts anew;
 * in redo, what redo keeps of its files, too.
 */
static void
journal_drop_database(Oid dboid)
{
	JournalShared *js = shared();
	FileTag		tag;

	/* (in redo: copies from it synced first, and none waits on it) */
	if (InRecovery)
	{
		copied_sync();
		redo_removed(dboid, InvalidOid, true, false, 0);
	}

	jindex_forget(dboid);
	if (node_cache() != NULL && PROLLY_HAS(node_cache(), forget_database))
		node_cache()->forget_database(dboid);
	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < CHECKED_ENDS; i++)
	{
		if (js->checked[i].dboid == dboid)
			memset(&js->checked[i], 0, sizeof(CheckedEnd));
	}
	SpinLockRelease(&js->mutex);
	memset(&tag, 0, sizeof(tag));
	tag.handler = SYNC_HANDLER_PROLLY_CHUNKS;
	tag.forknum = (int16) prolly_chunkstore_journal.info.impl_id;
	tag.rlocator.dbOid = dboid;
	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true);
}

/* fsync the segment or table file a sync request names. */
static int
journal_sync_file(const FileTag *ftag, char *path)
{
	char	   *dir = GetDatabasePath(ftag->rlocator.dbOid, ftag->rlocator.spcOid);
	char	   *file;
	int			fd;
	int			save_errno;

	if (ftag->rlocator.relNumber == SYNC_TABLE_FILE)
		file = table_file_path(dir, ftag->segno);
	else
		file = segment_path(ftag->rlocator.dbOid, ftag->rlocator.spcOid,
							ftag->segno);
	strlcpy(path, file, MAXPGPATH);
	pfree(file);
	pfree(dir);
	INJECTION_POINT("prolly-checkpoint-chunk-file", NULL);
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;	/* (gone: retired since) */
	if (pg_fsync(fd) < 0)
	{
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
		return -1;
	}
	CloseTransientFile(fd);
	return 0;
}

/*
 * What a crash left in database directory dir (of dboid, in spcoid), as
 * crash recovery ends: the files no manifest names (a writer's or a
 * collection's, cut short), those retiring (no reader of them is left),
 * the dropped lists of collections (no transaction that straddled their
 * swap is left), and files written aside.  Each removal of a store file is
 * WAL-logged, as a writer's are, so a standby removes its copy too.
 */
static void
remove_leftovers(const char *dir, Oid dboid, Oid spcoid)
{
	ProllyChunkStore s;
	Manifest	m;
	DIR		   *d;
	struct dirent *de;
	char	   *path = dir_file(dir, MANIFEST_FILE);
	struct stat st;
	bool		has_store = stat(path, &st) == 0;

	pfree(path);
	path = dir_file(dir, JOURNAL_FILE);
	has_store |= stat(path, &st) == 0;
	pfree(path);

	/*
	 * (or a store's file alone: a store's first table file is written
	 * before its first manifest, and needs no journal)
	 */
	if (!has_store)
	{
		d = AllocateDir(dir);
		while (!has_store && (de = ReadDir(d, dir)) != NULL)
			has_store = strncmp(de->d_name, "prolly_table_", 13) == 0 ||
				strncmp(de->d_name, SEGMENT_PREFIX, strlen(SEGMENT_PREFIX)) == 0;
		FreeDir(d);
	}
	if (!has_store)
		return;
	memset(&s, 0, sizeof(s));
	s.dboid = dboid;
	s.spcoid = spcoid;
	PG_TRY();
	{
		(void) read_manifest(dir, &m);
	}
	PG_CATCH();
	{
		/* (a store this server refuses is left as it is) */
		EmitErrorReport();
		FlushErrorState();
		return;
	}
	PG_END_TRY();
	remove_unnamed(&s, dir, &m);
	for (int i = 0; i < m.nretiring; i++)
	{
		char	   *table = table_file_path(dir, m.retiring[i]);
		char	   *segment = segment_path(dboid, spcoid, m.retiring[i]);

		if (stat(table, &st) == 0)
			table_file_unlink(&s, dir, m.retiring[i], false);
		if (stat(segment, &st) == 0)
			table_file_unlink(&s, dir, m.retiring[i], true);
		pfree(table);
		pfree(segment);
	}
	d = AllocateDir(dir);
	while ((de = ReadDir(d, dir)) != NULL)
	{
		if (strncmp(de->d_name, "prolly_gc_dropped_", 18) == 0 ||
			strcmp(de->d_name, MANIFEST_TEMP) == 0)
		{
			path = dir_file(dir, de->d_name);
			if (unlink(path) != 0 && errno != ENOENT)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			pfree(path);
		}
	}
	FreeDir(d);
}

/* every database directory under parent (of tablespace spcoid), to fn */
static void
dirs_under(const char *parent, Oid spcoid, DatabaseDirFn fn)
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
		fn(sub, (Oid) strtoul(de->d_name, NULL, 10), spcoid);
		pfree(sub);
	}
	FreeDir(dir);
}

/* every database directory, in every tablespace, to fn */
static void
each_database_dir(DatabaseDirFn fn)
{
	DIR		   *tsdir;
	struct dirent *de;

	dirs_under("base", DEFAULTTABLESPACE_OID, fn);
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
		dirs_under(sub, (Oid) strtoul(de->d_name, NULL, 10), fn);
		pfree(sub);
	}
	FreeDir(tsdir);
}

static void
journal_recovered(void)
{
	each_database_dir(remove_leftovers);
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
	ProllyHash *wanted;
	int			nwanted = 0;

	if (s->through_hint != NULL)
	{
		if (n > 0)
			s->through_hint(s->through_arg, hashes, n);
		return true;
	}
	if (n < 2)
		return true;
	/* (what the walk reads anyway: not those pending or read ahead) */
	wanted = palloc(sizeof(ProllyHash) * n);
	for (int i = 0; i < n; i++)
	{
		ProllyHash	key;

		hash_key(&key, &hashes[i]);
		if (hash_search(s->pending_index, &key, HASH_FIND, NULL) == NULL &&
			(s->ahead == NULL || hash_search(s->ahead, &key, HASH_FIND, NULL) == NULL))
			wanted[nwanted++] = key;
	}
	if (nwanted > 1)
	{
		AheadFill	af = {s, wanted};

		journal_get_many(s, wanted, nwanted, ahead_put, &af);
	}
	pfree(wanted);
	return true;
}

/*
 * A table file's chunks were checked against the store as the transaction
 * found it (a transfer's walk declines what the store has): if a swap has
 * happened since the transaction began, what it trusted may be gone, and
 * the transaction is retried.
 */
static void
add_table_file(ProllyChunkStore *s, ProllyChunkNext next, void *arg,
			   bool stored)
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
	write_table_file(s, &m, keep, 0, 0, 0, next, arg, stored, NULL, false, 0,
					 NULL);
	pfree(keep);
}

/* a settle()'s queue of chunks to visit: an array, grown as needed */
typedef struct RefQueue
{
	ProllyHash **queue;
	int		   *tail;
	int		   *max;
} RefQueue;

static void
queue_ref(const ProllyHash *ref, void *arg)
{
	RefQueue   *rq = arg;

	if (*rq->tail == *rq->max)
	{
		*rq->max *= 2;
		*rq->queue = repalloc_huge(*rq->queue, sizeof(ProllyHash) * *rq->max);
	}
	(*rq->queue)[(*rq->tail)++] = *ref;
}

static int	settled_file(ProllyChunkStore *s, const Manifest *m,
						 const ProllyHash *key);

/* a settle()'s chunks, in the order found */
typedef struct SettleSource
{
	ProllyChunkStore *s;
	List	   *hashes;			/* ProllyHash *: the group's */
	ListCell   *cur;
	HTAB	   *settling;		/* SettledChunk: every group's so far */
	const Manifest *m;
	ProllySettleAbsent absent;	/* the caller's, for what isn't here */
	void	   *absent_arg;
	uint32		group_lo;		/* the group's first generation */
} SettleSource;

/* a chunk settle() writes, and the generation of the file it is in */
typedef struct SettledChunk
{
	ProllyHash	hash;			/* key */
	uint32		epoch;
} SettledChunk;

/*
 * The generation of a chunk a chunk settled names: the first of its own
 * group's (none to list) or an older group's, if it is settled too; else
 * the first of the settled file's it is in.
 */
static uint32
settle_resolve(void *arg, const ProllyHash *target, bool *always)
{
	SettleSource *ss = arg;
	ProllyHash	key;
	SettledChunk *sc;
	int			f;

	hash_key(&key, target);
	sc = hash_search(ss->settling, &key, HASH_FIND, NULL);
	if (sc != NULL)
		return sc->epoch;
	f = settled_file(ss->s, ss->m, &key);
	if (f >= 0)
		return ss->m->files[f].epoch_lo;

	/*
	 * Not here: a severed fragment's top, which a repair later puts back
	 * in the generation its fragment says, listed by that one.
	 */
	if (ss->absent != NULL && !stored_has(ss->s, &key))
	{
		uint32		epoch = ss->absent(ss->absent_arg, target, ss->group_lo);

		if (epoch > 0)
			*always = true;
		return epoch;
	}
	return 0;
}

static bool
settle_next(void *arg, ProllyHash *hash, StringInfo data)
{
	SettleSource *ss = arg;

	while (ss->cur != NULL)
	{
		ProllyHash *h = lfirst(ss->cur);

		ss->cur = lnext(ss->hashes, ss->cur);
		resetStringInfo(data);
		if (!journal_get(ss->s, h, data))
			continue;
		*hash = *h;
		return true;
	}
	return false;
}

/*
 * Is the chunk (key) the store's but in no settled file: in the journal,
 * or a table file not settled?  (One in both is too: settling it again
 * costs a copy, never a chunk.)
 */
static bool
unsettled_has(ProllyChunkStore *s, const ProllyHash *key)
{
	ListCell   *lc;

	if (jindex_has(s->ix, key) ||
		hash_search(s->pending_index, key, HASH_FIND, NULL) != NULL)
		return true;
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);
		uint64		offset;
		uint32		len;

		if (!t->settled &&
			PROLLY_CALL(table_format(), find, t->tf, key, &offset, &len))
			return true;
	}
	return false;
}

/*
 * The settled table file of m's (by its place in m) that has the chunk
 * (key), or -1 if none has.
 */
static int
settled_file(ProllyChunkStore *s, const Manifest *m, const ProllyHash *key)
{
	ListCell   *lc;
	int			oldest = -1;

	/* (the copy of the oldest generation, if several files hold one) */
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);
		uint64		offset;
		uint32		len;

		if (!t->settled ||
			(oldest >= 0 && t->epoch_lo >= m->files[oldest].epoch_lo) ||
			!PROLLY_CALL(table_format(), find, t->tf, key, &offset, &len))
			continue;
		for (int i = 0; i < m->nfiles; i++)
		{
			if (m->files[i].id == t->id)
				oldest = i;
		}
	}
	return oldest;
}

/* settle()'s: a root, and the generation the settled files list it by */
typedef struct SettleRoot
{
	ProllyHash	hash;			/* key */
	uint32		epoch;			/* (0: none lists it) */
	uint32		given;			/* the caller's (0: none) */
} SettleRoot;

static void
note_listed_root(const ProllyHash *target, const ProllyHash *source,
				 uint32 epoch, void *arg)
{
	HTAB	   *roots = arg;
	ProllyHash	key;
	SettleRoot *r;

	hash_key(&key, target);
	r = hash_search(roots, &key, HASH_FIND, NULL);
	if (r != NULL && epoch > r->epoch)
		r->epoch = epoch;
}

static int
settle_root_cmp(const void *a, const void *b)
{
	const SettleRoot *x = a;
	const SettleRoot *y = b;

	return x->epoch < y->epoch ? -1 : x->epoch > y->epoch;
}

/*
 * Settle what roots reach: every chunk they reach that no settled file
 * holds, down to those one does (and what isn't here: a severed parent),
 * written to settled table files.  For chunks that only a settled file's
 * chunks may name: a fragment of history a repair received, which the
 * boundary commits name.  A default collection doesn't read settled
 * files, so it would never find them otherwise, and would free them.
 *
 * Each goes back to the generation it was severed from: the one the
 * sections of the files naming a root list it by (a collection lists a
 * severed parent by its generation as it drops it).  Roots of one
 * generation are settled together, oldest first, each reaching what an
 * older one didn't; what they reach that a settled file of a newer
 * generation holds (a tree brought back since the sever) is settled with
 * them too, so a file names nothing newer than itself.  Roots none lists
 * go with the newest of those listed; if none is, all go to the
 * generation of the newest settled chunk they name.
 *
 * Costs what it settles: a chunk is looked up in the journal's index and
 * the few files not settled; only where the walk stops are the settled
 * files asked which has it.  The settled files' sections are read once.
 */
static void
journal_settle(ProllyChunkStore *s, const ProllyHash *roots,
			   const uint32 *epochs, int nroots, ProllySettleAbsent absent,
			   ProllySettleNamedBy named_by, void *arg)
{
	Manifest	m;
	bool	   *keep;
	SettleSource ss = {s, NIL, NULL, NULL, NULL};
	XrefBuild	xb = {0};
	ProllyHash *queue;
	int			max = Max(nroots, 64);
	HASHCTL		ctl;
	HTAB	   *seen;
	HTAB	   *listed;
	SettleRoot *order;
	int			nordered = 0;
	uint32		newest_listed = 0;
	StringInfoData data;
	ListCell   *lc;

	xact_note(s);
	lock_manifest(s, &m);
	if (m.gc_generation != s->xact_generation)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("the chunk store was collected while the transaction ran"),
				 errhint("Try the transaction again.")));
	load_tables(s, true);
	scan_journal(s);
	ctl.keysize = sizeof(ProllyHash);
	ctl.hash = chunk_hash_key_hash;
	ctl.hcxt = CurrentMemoryContext;
	ctl.entrysize = sizeof(ProllyHash);
	seen = hash_create("prolly settle", 1024, &ctl,
					   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	ctl.entrysize = sizeof(SettledChunk);
	ss.settling = hash_create("prolly settling", 1024, &ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	ctl.entrysize = sizeof(SettleRoot);
	listed = hash_create("prolly settle roots", 16, &ctl,
						 HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	ss.m = &m;
	ss.absent = absent;
	ss.absent_arg = arg;

	/* the generations the roots go back to, or are listed by */
	for (int i = 0; i < nroots; i++)
	{
		ProllyHash	key;
		SettleRoot *r;
		bool		found;

		if (roots[i].len == 0)
			continue;
		hash_key(&key, &roots[i]);
		r = hash_search(listed, &key, HASH_ENTER, &found);
		if (!found)
			r->epoch = 0;
		r->given = epochs ? epochs[i] : 0;
		/* (for testing: as a repair that settled it wrong would) */
		if (debug_prolly_settle_epoch > 0)
			r->given = (uint32) debug_prolly_settle_epoch;
	}
	if (debug_prolly_settle_epoch > 0)
		ss.absent = NULL;

	/*
	 * What names a root from outside lists it by the generation it goes
	 * back to: taken from there; checked against it below.
	 */
	if (named_by != NULL && debug_prolly_settle_epoch == 0)
	{
		HASH_SEQ_STATUS status;
		SettleRoot *r;

		hash_seq_init(&status, listed);
		while ((r = hash_seq_search(&status)) != NULL)
		{
			ProllyHash *by = NULL;
			int			nby = named_by(arg, &r->hash, &by);

			for (int b = 0; b < nby; b++)
			{
				int			f = settled_file(s, &m, &by[b]);
				TableFile  *t = NULL;
				HTAB	   *cache = NULL;
				bool		always = false;
				uint32		e;

				if (f < 0)
					continue;
				foreach(lc, s->tables)
					if (((TableFile *) lfirst(lc))->id == m.files[f].id)
						t = lfirst(lc);
				e = listed_epoch(t, &cache, CurrentMemoryContext, &r->hash,
								 &always);
				if (cache)
					hash_destroy(cache);
				if (e > 0)
					r->given = e;
			}
			if (by)
				pfree(by);
		}
	}
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);
		XrefSection x;

		if (t->settled && xrefs_read(t, &x))
		{
			xrefs_visit(&x, 0, PG_UINT32_MAX, note_listed_root, listed);
			xrefs_free(&x);
		}
	}
	order = palloc(sizeof(SettleRoot) * Max(nroots, 1));
	{
		HASH_SEQ_STATUS status;
		SettleRoot *r;

		hash_seq_init(&status, listed);
		while ((r = hash_seq_search(&status)) != NULL)
		{
			order[nordered] = *r;
			if (r->given > 0)
				order[nordered].epoch = r->given;
			newest_listed = Max(newest_listed, order[nordered].epoch);
			nordered++;
		}
	}
	for (int i = 0; i < nordered; i++)
	{
		if (order[i].epoch == 0)
			order[i].epoch = newest_listed;
	}
	qsort(order, nordered, sizeof(SettleRoot), settle_root_cmp);

	initStringInfo(&data);
	initStringInfo(&xb.chunk);
	queue = MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(ProllyHash) * max);
	keep = palloc(sizeof(bool) * Max(m.nfiles, 1));
	for (int g = 0; g < nordered;)
	{
		uint32		epoch = order[g].epoch;
		uint32		epoch_lo = epoch;
		uint32		epoch_hi = epoch;
		int			newest = -1;	/* (none listed: the newest named) */
		int			head = 0;
		int			tail = 0;
		List	   *group = NIL;

		int			first = g;

		for (; g < nordered && order[g].epoch == epoch; g++)
			queue[tail++] = order[g].hash;

		/* (the interval of a file holding the generation, if one does) */
		for (int i = 0; i < m.nfiles && epoch > 0; i++)
		{
			if (m.files[i].epoch_lo > 0 && m.files[i].epoch_lo <= epoch &&
				m.files[i].epoch_hi >= epoch)
			{
				epoch_lo = m.files[i].epoch_lo;
				epoch_hi = m.files[i].epoch_hi;
			}
		}

		/*
		 * Each chunk naming a root from outside, if settled, as new as the
		 * root goes back to, and listing it by that generation if newer;
		 * else nothing is settled: a retire relies on it.
		 */
		for (int i = first; i < g && named_by != NULL && epoch > 0; i++)
		{
			ProllyHash *by = NULL;
			int			nby = named_by(arg, &order[i].hash, &by);

			for (int b = 0; b < nby; b++)
			{
				int			f = settled_file(s, &m, &by[b]);
				TableFile  *t = NULL;
				HTAB	   *cache = NULL;
				bool		always = false;
				uint32		e;

				if (f < 0)
					continue;
				foreach(lc, s->tables)
					if (((TableFile *) lfirst(lc))->id == m.files[f].id)
						t = lfirst(lc);
				e = listed_epoch(t, &cache, CurrentMemoryContext,
								 &order[i].hash, &always);
				if (cache)
					hash_destroy(cache);
				if (m.files[f].epoch_hi < epoch_lo ||
					(m.files[f].epoch_lo > epoch_hi &&
					 (e < epoch_lo || e > epoch_hi)))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("chunk %s would be settled in generations %u to %u, which chunk %s naming it, of generations %u to %u, doesn't list it by",
									prolly_hash_hex(&order[i].hash), epoch_lo,
									epoch_hi, prolly_hash_hex(&by[b]),
									m.files[f].epoch_lo, m.files[f].epoch_hi),
							 errdetail("Nothing was settled.")));
			}
			if (by)
				pfree(by);
		}
		while (head < tail)
		{
			ProllyHash	h = queue[head++];
			ProllyHash	key;
			bool		found;
			RefQueue	rq;

			CHECK_FOR_INTERRUPTS();
			if (h.len == 0)
				continue;
			hash_key(&key, &h);
			(void) hash_search(seen, &key, HASH_ENTER, &found);
			if (found)
				continue;
			if (!unsettled_has(s, &key))
			{
				/* (settled, or not here at all: the walk stops) */
				int			f = settled_file(s, &m, &key);

				if (f < 0)
					continue;
				if (epoch == 0 || m.files[f].epoch_lo <= epoch_hi ||
					debug_prolly_settle_epoch > 0)
				{
					if (newest < 0 ||
						m.files[f].epoch_lo > m.files[newest].epoch_lo)
						newest = f;
					continue;
				}
				/* (of a newer generation than the group's: settled with it) */
			}
			resetStringInfo(&data);
			if (!journal_get(s, &h, &data))
				continue;
			group = lappend(group, memcpy(palloc(sizeof(ProllyHash)), &h,
										  sizeof(ProllyHash)));
			/* (the queue's room reused once half of it is taken) */
			if (head > max / 2)
			{
				memmove(queue, queue + head, sizeof(ProllyHash) * (tail - head));
				tail -= head;
				head = 0;
			}
			rq.queue = &queue;
			rq.tail = &tail;
			rq.max = &max;
			prolly_chunk_references(data.data, data.len, queue_ref, &rq);
		}

		/*
		 * None listed: the generations of the newest settled chunk named
		 * (what names the roots names those too); else of the oldest
		 * settled file; else the next collection's.
		 */
		if (epoch == 0)
		{
			if (newest < 0)
			{
				for (int i = 0; i < m.nfiles; i++)
				{
					if (m.files[i].epoch_lo > 0 &&
						(newest < 0 || m.files[i].epoch_lo < m.files[newest].epoch_lo))
						newest = i;
				}
			}
			if (newest >= 0)
			{
				epoch_lo = m.files[newest].epoch_lo;
				epoch_hi = m.files[newest].epoch_hi;
			}
			else
				epoch_lo = epoch_hi = m.gc_generation + 1;
		}
		foreach(lc, group)
		{
			ProllyHash	key;
			SettledChunk *sc;

			hash_key(&key, lfirst(lc));
			sc = hash_search(ss.settling, &key, HASH_ENTER, NULL);
			sc->epoch = epoch_lo;
		}
		ss.hashes = group;
		ss.cur = list_head(group);
		ss.group_lo = epoch_lo;
		xb.resolve = settle_resolve;
		xb.arg = &ss;
		xb.lo = epoch_lo;
		xb.hi = epoch_hi;
		for (int i = 0; i < m.nfiles; i++)
			keep[i] = true;
		if (group != NIL)
		{
			write_table_file(s, &m, keep,
							 MANIFEST_FILE_SETTLED | MANIFEST_FILE_REPAIRED,
							 epoch_lo,
							 epoch_hi, settle_next, &ss, false, NULL, false, 0,
							 &xb);
			/* (the manifest written names the file: the next adds to it) */
			{
				char	   *dir = GetDatabasePath(s->dboid, s->spcoid);

				(void) read_manifest(dir, &m);
				pfree(dir);
				keep = repalloc(keep, sizeof(bool) * Max(m.nfiles, 1));
			}
		}
		list_free_deep(group);
	}
	pfree(keep);
	pfree(queue);
	pfree(order);
	pfree(data.data);
	pfree(xb.chunk.data);
	hash_destroy(listed);
	hash_destroy(ss.settling);
	hash_destroy(seen);
}

static void
journal_add_table_file(ProllyChunkStore *s, ProllyChunkNext next, void *arg)
{
	add_table_file(s, next, arg, false);
}

/* The same, of chunks next() gives framed as stored: written as they are. */
static void
journal_add_stored_table_file(ProllyChunkStore *s, ProllyChunkNext next,
							  void *arg)
{
	add_table_file(s, next, arg, true);
}

/* ------------------------------------------------------------------------
 * Whole files: a clone's
 * ------------------------------------------------------------------------
 */

/* The id of the file received for key. */
static uint64
received_id(const char *key)
{
	uint64		h = hash_bytes_extended((const unsigned char *) key, strlen(key), 0);

	return h | RECEIVED_ID;
}

/*
 * A received file that couldn't be stat'ed or opened (errno says why),
 * closed: raised, unless it is missing.
 */
static void
received_open_failed(TableFile *t)
{
	int			save_errno = errno;
	char	   *path = pstrdup(t->path);

	close_table(t);
	errno = save_errno;
	if (errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	pfree(path);
}

/*
 * The received file id of database directory dir, opened in cxt: NULL if
 * it isn't there whole (missing, or cut short: the format finds no whole
 * file in its bytes).  Any other error (a file that can't be opened or
 * read, running out of memory, a cancel) is raised, the file closed: it
 * says nothing of whether the file is whole.
 */
static TableFile *
open_received(const char *dir, uint64 id, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	TableFile  *t = palloc0(sizeof(TableFile));
	struct stat st;

	t->id = id;
	t->path = table_file_path(dir, id);
	t->file = -1;
	MemoryContextSwitchTo(old);
	if (stat(t->path, &st) != 0)
	{
		received_open_failed(t);
		return NULL;
	}
	t->size = st.st_size;
	t->file = PathNameOpenFile(t->path, O_RDONLY | PG_BINARY);
	if (t->file < 0)
	{
		received_open_failed(t);
		return NULL;
	}
	PG_TRY();
	{
		INJECTION_POINT("prolly-received-open", NULL);
		t->tf = PROLLY_CALL(table_format(), open, table_file_read,
							table_file_map, t, t->size, cxt);
	}
	PG_CATCH();
	{
		t->tf = NULL;
		close_table(t);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (t->tf == NULL)
	{
		close_table(t);
		return NULL;
	}
	return t;
}

/* The ids of the received files in database directory dir. */
static List *
received_ids(const char *dir)
{
	DIR		   *d = AllocateDir(dir);
	struct dirent *de;
	List	   *ids = NIL;

	while ((de = ReadDir(d, dir)) != NULL)
	{
		char	   *end;
		uint64		id;

		if (strncmp(de->d_name, RECEIVED_PREFIX, strlen(RECEIVED_PREFIX)) != 0)
			continue;
		id = strtou64(de->d_name + strlen(RECEIVED_PREFIX), &end, 16);
		if (*end != '\0')
			continue;
		ids = lappend(ids, (void *) palloc(sizeof(uint64)));
		*(uint64 *) llast(ids) = id | RECEIVED_ID;
	}
	FreeDir(d);
	return ids;
}

/* Forget the received files opened: what is on disk changed. */
static void
received_changed(ProllyChunkStore *s)
{
	close_tables(s->received);
	s->received = NIL;
	s->received_open = false;
}

/* The received files on disk, whole, opened. */
static List *
received_tables(ProllyChunkStore *s)
{
	char	   *dir;
	List	   *ids;
	List	   *volatile opened = NIL;

	if (s->received_open)
		return s->received;
	dir = GetDatabasePath(s->dboid, s->spcoid);
	ids = received_ids(dir);
	/* (listed only once all are opened: an error leaves none listed) */
	PG_TRY();
	{
		foreach_ptr(uint64, id, ids)
		{
			TableFile  *t = open_received(dir, *id, TopMemoryContext);

			if (t)
			{
				MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

				opened = lappend(opened, t);
				MemoryContextSwitchTo(old);
			}
		}
	}
	PG_CATCH();
	{
		close_tables(opened);
		PG_RE_THROW();
	}
	PG_END_TRY();
	list_free_deep(ids);
	pfree(dir);
	s->received = opened;
	s->received_open = true;
	return s->received;
}

static void
journal_receive_file(ProllyChunkStore *s, const char *key, ProllyChunkNext next,
					 void *arg)
{
	uint32		nchunks;
	uint64		size;
	bool		full;

	/* (a remote's file copied whole: one file, whatever its size) */
	received_changed(s);
	(void) write_file(s, received_id(key), next, arg, true, NULL, 0, NULL,
					  &full, &nchunks, &size);
}

static bool
journal_received_file(ProllyChunkStore *s, const char *key, uint32 *nchunks)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	TableFile  *t = open_received(dir, received_id(key), CurrentMemoryContext);

	pfree(dir);
	if (t == NULL)
		return false;
	*nchunks = (uint32) PROLLY_CALL(table_format(), count, t->tf);
	close_table(t);
	return true;
}

/* A chunk's bytes in a table file: read, decoded or (stored) as stored. */
static void
table_chunk(TableFile *t, uint64 offset, uint32 len, bool stored, StringInfo out)
{
	char	   *bytes = palloc(len);

	if (!table_file_read(t, offset, bytes, len))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, t->path)));
	pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_FILE_READS, 1);
	if (stored)
		PROLLY_CALL(table_format(), read_stored, t->tf, bytes, len, out);
	else
		PROLLY_CALL(table_format(), decode, t->tf, bytes, len, out);
	pfree(bytes);
}

static bool
journal_received_get(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	ProllyHash	key;

	hash_key(&key, hash);
	foreach_ptr(TableFile, t, received_tables(s))
	{
		uint64		offset;
		uint32		len;

		if (!PROLLY_CALL(table_format(), find, t->tf, &key, &offset, &len))
			continue;
		if (out)
			table_chunk(t, offset, len, false, out);
		return true;
	}
	return false;
}

static void
journal_received_chunks(ProllyChunkStore *s, ProllyChunkStoredVisit visit,
						void *arg)
{
	StringInfoData frame;

	initStringInfo(&frame);
	foreach_ptr(TableFile, t, received_tables(s))
	{
		int			n = PROLLY_CALL(table_format(), count, t->tf);

		for (int i = 0; i < n; i++)
		{
			ProllyHash	h;
			uint64		offset;
			uint32		len;

			PROLLY_CALL(table_format(), entry, t->tf, i, &h, &offset, &len);
			resetStringInfo(&frame);
			table_chunk(t, offset, len, true, &frame);
			visit(&h, frame.data, frame.len, arg);
			CHECK_FOR_INTERRUPTS();
		}
	}
	pfree(frame.data);
}

/*
 * Name the received files in the manifest, beside those it names, at
 * once; the received files not whole removed.  The manifest's lock is
 * held until the transaction ends.
 */
static void
journal_adopt_received(ProllyChunkStore *s)
{
	Manifest	m;
	Manifest	nm;
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	List	   *ids;
	List	   *tables = received_tables(s);

	xact_note(s);
	lock_manifest(s, &m);
	if (m.gc_generation != s->xact_generation)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("the chunk store was collected while the transaction ran"),
				 errhint("Try the transaction again.")));
	nm = m;
	scan_journal(s);
	nm.root = s->root;
	nm.files = palloc(sizeof(ManifestFile) * (m.nfiles + list_length(tables) + 1));
	memcpy(nm.files, m.files, sizeof(ManifestFile) * m.nfiles);
	foreach_ptr(TableFile, t, tables)
	{
		bool		named = false;

		for (int i = 0; i < m.nfiles; i++)
			named |= m.files[i].id == t->id;
		if (named)
			continue;
		nm.files[nm.nfiles].id = t->id;
		nm.files[nm.nfiles].nchunks = PROLLY_CALL(table_format(), count, t->tf);
		nm.files[nm.nfiles].size = t->size;
		nm.files[nm.nfiles].flags = 0;
		nm.files[nm.nfiles].epoch_lo = nm.files[nm.nfiles].epoch_hi = 0;
		nm.files[nm.nfiles].garbage = 0;
		nm.nfiles++;
	}
	replace_manifest(s, &nm, XLOG_CHUNKS_MANIFEST);

	/* (those not whole: never to be adopted) */
	ids = received_ids(dir);
	foreach_ptr(uint64, id, ids)
	{
		bool		named = false;

		for (int i = 0; i < nm.nfiles; i++)
			named |= nm.files[i].id == *id;
		if (!named)
			table_file_unlink(s, dir, *id, false);
	}
	list_free_deep(ids);
	received_changed(s);
	load_tables(s, true);
	pfree(nm.files);
	pfree(dir);
}

/* Remove the received files the manifest doesn't name. */
static void
journal_forget_received(ProllyChunkStore *s, const char *const *keep, int nkeep)
{
	Manifest	m;
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	List	   *ids;

	received_changed(s);
	lock_manifest(s, &m);
	ids = received_ids(dir);
	foreach_ptr(uint64, id, ids)
	{
		bool		named = false;

		for (int i = 0; i < m.nfiles; i++)
			named |= m.files[i].id == *id;
		for (int i = 0; i < nkeep && !named; i++)
			named = received_id(keep[i]) == *id;
		if (!named)
			table_file_unlink(s, dir, *id, false);
	}
	list_free_deep(ids);
	pfree(dir);
}

static int
hash_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(ProllyHash));
}

/* the journal's chunks, as file_chunks() reads them: their hashes first */
typedef struct JournalChunks
{
	ProllyHash *hashes;
	int			n;
	int			max;
} JournalChunks;

static void
note_journal_chunk(const ProllyHash *hash, uint64 segment, int ordinal, void *arg)
{
	JournalChunks *jc = arg;

	if (jc->n == jc->max)
	{
		jc->max = Max(64, jc->max * 2);
		jc->hashes = jc->hashes ? repalloc_huge(jc->hashes, sizeof(ProllyHash) * jc->max) :
			MemoryContextAllocHuge(CurrentMemoryContext, sizeof(ProllyHash) * jc->max);
	}
	jc->hashes[jc->n++] = *hash;
}

/*
 * The chunks of table file name (or, NULL, of the journal: each chunk
 * once), as stored, in the file's order (the journal's: its index's).
 */
static void
journal_file_chunks(ProllyChunkStore *s, const char *name,
					ProllyChunkStoredVisit visit, void *arg)
{
	StringInfoData frame;

	initStringInfo(&frame);
	if (name == NULL)
	{
		JournalChunks jc = {0};
		ProllyHash	prev = {0};

		scan_journal(s);
		jindex_visit(s->ix, note_journal_chunk, &jc);
		qsort(jc.hashes, jc.n, sizeof(ProllyHash), hash_cmp);
		for (int i = 0; i < jc.n; i++)
		{
			if (i > 0 && memcmp(&jc.hashes[i], &prev, sizeof(ProllyHash)) == 0)
				continue;
			prev = jc.hashes[i];
			resetStringInfo(&frame);
			if (!journal_get_stored(s, &jc.hashes[i], &frame))
				continue;		/* (collected since) */
			visit(&jc.hashes[i], frame.data, frame.len, arg);
			CHECK_FOR_INTERRUPTS();
		}
		if (jc.hashes)
			pfree(jc.hashes);
	}
	else
	{
		TableFile  *found = NULL;

		load_tables(s, false);
		foreach_ptr(TableFile, t, s->tables)
		{
			char	   *n = table_file_name(t->id);

			if (strcmp(n, name) == 0)
				found = t;
			pfree(n);
		}
		if (found == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("the chunk store has no table file \"%s\"", name)));
		for (int i = 0; i < PROLLY_CALL(table_format(), count, found->tf); i++)
		{
			ProllyHash	h;
			uint64		offset;
			uint32		len;

			PROLLY_CALL(table_format(), entry, found->tf, i, &h, &offset, &len);
			resetStringInfo(&frame);
			table_chunk(found, offset, len, true, &frame);
			visit(&h, frame.data, frame.len, arg);
			CHECK_FOR_INTERRUPTS();
		}
	}
	pfree(frame.data);
}

/* conjoin()'s source: every chunk of the table files, once */
typedef struct ConjoinSource
{
	ProllyChunkStore *s;
	List	   *tables;			/* TableFile *, opened for this */
	ListCell   *cur;
	int			i;				/* the next entry of cur's */
	HTAB	   *seen;
	const ProllyHashRoutine *hash;	/* each chunk checked by */
	StringInfoData bytes;		/* the chunk given last, as its file has it */
	StringInfoData chunk;		/* and decoded, to check it */
	CopyFrom	from;			/* and where it is */
	const Manifest *m;			/* the manifest conjoined in */
	const bool *keep;			/* its files not conjoined */
	TableFile **others;			/* and those, settled, opened (by file) */
	TableFile  *listed_of;		/* the file listed is of */
	HTAB	   *listed;			/* (listed_epoch()'s) */
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
			/* (as stored, logged as a copy; decoded only to check it) */
			resetStringInfo(&cs->bytes);
			enlargeStringInfo(&cs->bytes, len);
			bytes = cs->bytes.data;
			if (!table_file_read(t, offset, bytes, len))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, t->path)));
			prolly_tablefile_read_stored(fmt, t->tf, bytes, len, data);
			resetStringInfo(&cs->chunk);
			PROLLY_CALL(fmt, decode, t->tf, bytes, len, &cs->chunk);
			cs->from.src.kind = COPY_TABLE_BYTES;
			cs->from.src.id = t->id;
			cs->from.src.offset = offset;
			cs->from.src.len = len;
			cs->from.bytes = bytes;

			/* (read and written: a page missed and one dirtied) */
			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageMiss + VacuumCostPageDirty;
			vacuum_delay_point(false);

			/* checked as it is copied, as a collection's: its hash's bytes */
			{
				ProllyHashCtx *ctx = PROLLY_CALL(cs->hash, begin,
												 CurrentMemoryContext);
				ProllyHash	got;

				PROLLY_CALL(cs->hash, update, ctx, (const uint8 *) cs->chunk.data,
							cs->chunk.len);
				PROLLY_CALL(cs->hash, finish, ctx, &got);
				if (got.len != hash->len ||
					memcmp(got.bytes, hash->bytes, got.len) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("chunk in file \"%s\" doesn't match its hash",
									t->path)));
			}
			return true;
		}
		cs->cur = lnext(cs->tables, cs->cur);
		cs->i = 0;
	}
	return false;
}

/*
 * The generation of a chunk a conjoined chunk names: none (0) if it is in
 * one of the files conjoined; else the first of the settled file's it is
 * in.
 */
static uint32
conjoin_resolve(void *arg, const ProllyHash *target, bool *always)
{
	ConjoinSource *cs = arg;
	ProllyHash	key;
	ListCell   *lc;
	uint64		offset;
	uint32		len;

	hash_key(&key, target);
	foreach(lc, cs->tables)
	{
		TableFile  *t = lfirst(lc);

		if (PROLLY_CALL(table_format(), find, t->tf, &key, &offset, &len))
			return 0;
	}
	for (int i = 0; i < cs->m->nfiles; i++)
	{
		TableFile  *t;

		if (!cs->keep[i] || cs->m->files[i].epoch_lo == 0)
			continue;
		t = cs->others[i];
		if (t != NULL &&
			PROLLY_CALL(table_format(), find, t->tf, &key, &offset, &len))
			return cs->m->files[i].epoch_lo;
	}
	/* (gone: what the file the chunk comes from listed it as, if it did) */
	if (cs->cur != NULL)
	{
		TableFile  *t = lfirst(cs->cur);

		if (t != cs->listed_of)
		{
			if (cs->listed)
				hash_destroy(cs->listed);
			cs->listed = NULL;
			cs->listed_of = t;
		}
		return listed_epoch(t, &cs->listed, CurrentMemoryContext, &key,
							always);
	}
	return 0;
}

/* a manifest's file, for choosing what to conjoin */
typedef struct ConjoinCandidate
{
	int			file;			/* in the manifest */
	uint32		nchunks;
} ConjoinCandidate;

static int
candidate_cmp(const void *a, const void *b)
{
	const ConjoinCandidate *x = a;
	const ConjoinCandidate *y = b;

	if (x->nchunks != y->nchunks)
		return x->nchunks < y->nchunks ? -1 : 1;
	return x->file - y->file;
}

/*
 * A conjoin pass's bounds: the chunks its table of those seen may hold in
 * maintenance_work_mem, and the bytes it copies holding the manifest's
 * lock (which seals, collections and files received wait for).
 */
#define CONJOIN_SEEN_ENTRY	(sizeof(ProllyHash) + 32)
#define CONJOIN_MAX_BYTES	((uint64) 1024 * 1024 * 1024)

static int
candidate_epoch_cmp(const void *a, const void *b, void *arg)
{
	const Manifest *m = arg;
	const ManifestFile *x = &m->files[((const ConjoinCandidate *) a)->file];
	const ManifestFile *y = &m->files[((const ConjoinCandidate *) b)->file];

	if (x->epoch_lo != y->epoch_lo)
		return x->epoch_lo < y->epoch_lo ? -1 : 1;
	if (x->epoch_hi != y->epoch_hi)
		return x->epoch_hi < y->epoch_hi ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

/*
 * Do the settled files c[lo..hi] (by generation) hold every generation
 * between their first and last that any settled file does?  Then the file
 * they become may span them: no file left holds one of its generations.
 */
static bool
run_contiguous(const Manifest *m, const ConjoinCandidate *c, int n, int lo,
			   int hi)
{
	uint32		first = PG_UINT32_MAX;
	uint32		last = 0;

	for (int i = lo; i <= hi; i++)
	{
		first = Min(first, m->files[c[i].file].epoch_lo);
		last = Max(last, m->files[c[i].file].epoch_hi);
	}
	for (int i = 0; i < n; i++)
	{
		const ManifestFile *f = &m->files[c[i].file];

		if ((i < lo || i > hi) && f->epoch_lo <= last && f->epoch_hi >= first)
			return false;
	}
	return true;
}

/*
 * Choose the settled files a conjoin rewrites: a run of them in the order
 * of their generations, so that the file they become spans only
 * generations no other holds (each chunk keeps its generation, give or
 * take those it is conjoined with).  The two of fewest chunks together
 * begin it; then it takes in the neighbor of fewer chunks, by
 * choose_conjoinees()' rule and bounds.
 */
static bool
choose_settled_run(const Manifest *m, ConjoinCandidate *c, int n,
				   int max_files, uint64 max_chunks, bool *keep)
{
	int			best = -1;
	uint64		best_sum = 0;
	int			lo;
	int			hi;
	uint64		sum;
	uint64		bytes;

	qsort_arg(c, n, sizeof(ConjoinCandidate), candidate_epoch_cmp, (void *) m);
	for (int i = 0; i + 1 < n; i++)
	{
		uint64		pair = (uint64) c[i].nchunks + c[i + 1].nchunks;

		if ((best < 0 || pair < best_sum) && run_contiguous(m, c, n, i, i + 1))
		{
			best = i;
			best_sum = pair;
		}
	}
	if (best < 0)
		return false;
	lo = best;
	hi = best + 1;
	sum = best_sum;
	bytes = m->files[c[lo].file].size + m->files[c[hi].file].size;
	for (;;)
	{
		int			next = -1;
		int			left = lo - 1;
		int			right = hi + 1;
		int			taken = hi - lo + 1;

		if (left >= 0 && run_contiguous(m, c, n, left, hi))
			next = left;
		if (right < n && run_contiguous(m, c, n, lo, right) &&
			(next < 0 || c[right].nchunks < c[next].nchunks))
			next = right;
		if (next < 0)
			break;
		if (sum <= c[next].nchunks && m->nfiles - taken + 1 <= max_files)
			break;
		if (sum + c[next].nchunks > max_chunks ||
			bytes + m->files[c[next].file].size > CONJOIN_MAX_BYTES)
			break;
		sum += c[next].nchunks;
		bytes += m->files[c[next].file].size;
		if (next < lo)
			lo = next;
		else
			hi = next;
	}
	for (int i = lo; i <= hi; i++)
		keep[c[i].file] = false;
	return true;
}

/*
 * Choose the files of m a conjoin past max_files rewrites, as Dolt's
 * conjoiner does: of the settled files or the others (whichever are more,
 * so a settled file's chunks stay settled), the smallest by chunks, taking
 * the next while the ones taken add up to more than it, or while leaving
 * it would leave more than max_files; but, the first two aside, none that
 * would take the pass past its bounds, so a later pass takes the rest.
 * Settled files are taken as a run by generation (choose_settled_run()).
 * keep[] says which stay.  False if no two can be.
 */
static bool
choose_conjoinees(const Manifest *m, int max_files, bool *keep)
{
	uint64		max_chunks = (uint64) maintenance_work_mem * 1024 /
		CONJOIN_SEEN_ENTRY;
	uint64		bytes;
	ConjoinCandidate *c = palloc(sizeof(ConjoinCandidate) * m->nfiles);
	int			nsettled = 0;
	bool		settled;
	int			n = 0;
	int			taken;
	uint64		sum;

	for (int i = 0; i < m->nfiles; i++)
		nsettled += (m->files[i].flags & MANIFEST_FILE_SETTLED) != 0;
	settled = nsettled * 2 >= m->nfiles;
	if ((settled ? nsettled : m->nfiles - nsettled) < 2)
		settled = !settled;
	for (int i = 0; i < m->nfiles; i++)
	{
		keep[i] = true;
		if (((m->files[i].flags & MANIFEST_FILE_SETTLED) != 0) == settled)
		{
			c[n].file = i;
			c[n].nchunks = m->files[i].nchunks;
			n++;
		}
	}
	if (n < 2)
	{
		pfree(c);
		return false;
	}
	if (settled)
	{
		bool		chosen = choose_settled_run(m, c, n, max_files, max_chunks,
											   keep);

		pfree(c);
		return chosen;
	}
	qsort(c, n, sizeof(ConjoinCandidate), candidate_cmp);
	sum = (uint64) c[0].nchunks + c[1].nchunks;
	bytes = m->files[c[0].file].size + m->files[c[1].file].size;
	for (taken = 2; taken < n; taken++)
	{
		/* (the files left then: those not taken, and the new one) */
		if (sum <= c[taken].nchunks && m->nfiles - taken + 1 <= max_files)
			break;
		if (sum + c[taken].nchunks > max_chunks ||
			bytes + m->files[c[taken].file].size > CONJOIN_MAX_BYTES)
			break;
		sum += c[taken].nchunks;
		bytes += m->files[c[taken].file].size;
	}
	for (int i = 0; i < taken; i++)
		keep[c[i].file] = false;
	pfree(c);
	return true;
}

/*
 * Rewrite table files as one, which the manifest names instead of them,
 * settled if they all were: all of them (max_files 0), or past max_files,
 * the smallest (choose_conjoinees()).  Their chunks are copied as stored,
 * WAL-logged as copies of their bytes (#564).
 */
static void
journal_conjoin(ProllyChunkStore *s, int max_files)
{
	Manifest	m;
	bool	   *keep;
	ConjoinSource cs;
	HASHCTL		ctl;
	char	   *dir;
	ListCell   *lc;
	uint8		flags = MANIFEST_FILE_SETTLED;
	uint8		repaired = 0;
	uint32		epoch_lo = PG_UINT32_MAX;
	uint32		epoch_hi = 0;
	XrefBuild	xb = {0};

	lock_manifest(s, &m);
	if (m.nfiles < 2 || (max_files > 0 && m.nfiles <= max_files))
		return;
	keep = palloc0(sizeof(bool) * m.nfiles);
	if (max_files > 0 && !choose_conjoinees(&m, max_files, keep))
	{
		pfree(keep);
		return;
	}
	for (int i = 0; i < m.nfiles; i++)
	{
		if (keep[i])
			continue;
		flags &= m.files[i].flags;
		repaired |= m.files[i].flags & MANIFEST_FILE_REPAIRED;
		epoch_lo = Min(epoch_lo, m.files[i].epoch_lo);
		epoch_hi = Max(epoch_hi, m.files[i].epoch_hi);
	}
	/* (settled: the generations of those it conjoins, which are settled) */
	if ((flags & MANIFEST_FILE_SETTLED) == 0)
		epoch_lo = epoch_hi = 0;
	else
		flags |= repaired;

	/* the files as the lock's manifest names them, opened for this */
	dir = GetDatabasePath(s->dboid, s->spcoid);
	memset(&cs, 0, sizeof(cs));
	cs.s = s;
	cs.hash = prolly_content_hash();
	ctl.keysize = sizeof(ProllyHash);
	ctl.hash = chunk_hash_key_hash;
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	cs.seen = hash_create("prolly conjoin", 1024, &ctl,
						  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	initStringInfo(&cs.bytes);
	initStringInfo(&cs.chunk);

	/* (closed whatever happens: a file kept open is never given back) */
	PG_TRY();
	{
		for (int i = 0; i < m.nfiles; i++)
		{
			TableFile  *t;

			if (keep[i])
				continue;
			t = open_table(dir, &m.files[i], CurrentMemoryContext);
			if (t == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("table file %llu of the manifest is missing",
								(unsigned long long) m.files[i].id)));
			cs.tables = lappend(cs.tables, t);
		}
		cs.cur = list_head(cs.tables);

		/*
		 * (settled: what its chunks name in the settled files left, each
		 * older, listed by their generations)
		 */
		cs.m = &m;
		cs.keep = keep;
		cs.others = palloc0(sizeof(TableFile *) * m.nfiles);
		if (epoch_lo > 0)
		{
			for (int i = 0; i < m.nfiles; i++)
			{
				if (keep[i] && m.files[i].epoch_lo > 0)
				{
					cs.others[i] = open_table(dir, &m.files[i], CurrentMemoryContext);
					if (cs.others[i] == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("table file %llu of the manifest is missing",
										(unsigned long long) m.files[i].id)));
				}
			}
		}
		xb.resolve = conjoin_resolve;
		xb.arg = &cs;
		xb.lo = epoch_lo;
		xb.hi = epoch_hi;
		initStringInfo(&xb.chunk);
		write_table_file(s, &m, keep, flags, epoch_lo, epoch_hi, conjoin_next,
						 &cs, true, &cs.from,
						 true, (uint64) prolly_gc_file_size * 1024,
						 epoch_lo > 0 ? &xb : NULL);
	}
	PG_FINALLY();
	{
		foreach(lc, cs.tables)
		{
			release_table(lfirst(lc));
		}
		for (int i = 0; cs.others != NULL && i < m.nfiles; i++)
		{
			if (cs.others[i] != NULL)
				release_table(cs.others[i]);
		}
	}
	PG_END_TRY();
	pgstat_count_prolly(PGSTAT_PROLLY_CONJOINS, 1);
	ereport(DEBUG1,
			(errmsg_internal("chunk store of database %u: %d of %d table files conjoined",
							 s->dboid, list_length(cs.tables), m.nfiles)));
	hash_destroy(cs.seen);
	pfree(cs.bytes.data);
	pfree(cs.chunk.data);
	pfree(keep);
	pfree(dir);
}

/*
 * The table files as the manifest names them, with their counts, sizes and
 * flags: the files themselves aren't opened (nor their indexes read), so
 * a size-only question costs a manifest's read.
 */
static void
journal_table_files(ProllyChunkStore *s, ProllyChunkFileVisit visit, void *arg)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	Manifest	m;

	(void) read_manifest(dir, &m);
	for (int i = 0; i < m.nfiles; i++)
	{
		char	   *name = table_file_name(m.files[i].id);

		visit(name, (int) m.files[i].nchunks, m.files[i].size,
			  (m.files[i].flags & MANIFEST_FILE_SETTLED) != 0, arg);
		pfree(name);
	}
	pfree(dir);
}

/*
 * The table files as the manifest names them, with their generations.
 */
static void
journal_generations(ProllyChunkStore *s, ProllyChunkGenerationVisit visit,
					void *arg)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	Manifest	m;

	(void) read_manifest(dir, &m);
	for (int i = 0; i < m.nfiles; i++)
	{
		ProllyChunkGeneration g = {0};

		g.name = table_file_name(m.files[i].id);
		g.nchunks = m.files[i].nchunks;
		g.size = m.files[i].size;
		g.settled = (m.files[i].flags & MANIFEST_FILE_SETTLED) != 0;
		g.epoch_lo = m.files[i].epoch_lo;
		g.epoch_hi = m.files[i].epoch_hi;
		g.garbage = m.files[i].garbage;
		g.xrefs = file_xrefs(dir, &m.files[i]);
		visit(&g, arg);
		pfree((char *) g.name);
	}
	pfree(dir);
}

static void
journal_epochs(ProllyChunkStore *s, ProllyChunkEpochVisit visit, void *arg)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	Manifest	m;

	(void) read_manifest(dir, &m);
	for (int i = 0; i < m.nepochs; i++)
		visit(m.epochs[i].epoch, m.epochs[i].ended, arg);
	pfree(dir);
}

/* ------------------------------------------------------------------------
 * Checking generations
 * ------------------------------------------------------------------------
 */

bool		debug_prolly_verify_generations = DEFAULT_DEBUG_PROLLY_VERIFY_GENERATIONS;
int			debug_prolly_settle_epoch = 0;

/* a settled file, as checked */
typedef struct CheckedFile
{
	TableFile  *t;
	uint32		lo;
	uint32		hi;
	HTAB	   *listed;			/* XrefListed: what its section lists */
} CheckedFile;

typedef struct XrefListed
{
	ProllyHash	target;			/* key */
	int			n;
	uint32		epochs[8];		/* (the first few: one is enough) */
} XrefListed;

typedef struct VerifyRef
{
	ProllyChunkStore *s;
	CheckedFile *files;
	int			nfiles;
	int			self;			/* the file of the chunk checked */
	const ProllyHash *chunk;
} VerifyRef;

static void
note_listed(const ProllyHash *target, const ProllyHash *source, uint32 epoch,
			void *arg)
{
	HTAB	   *listed = arg;
	ProllyHash	key;
	XrefListed *x;
	bool		found;

	hash_key(&key, target);
	x = hash_search(listed, &key, HASH_ENTER, &found);
	if (!found)
		x->n = 0;
	if (x->n < lengthof(x->epochs))
		x->epochs[x->n++] = epoch;
}

static void
verify_ref(const ProllyHash *ref, void *arg)
{
	VerifyRef  *vr = arg;
	CheckedFile *self = &vr->files[vr->self];
	ProllyHash	key;
	bool		older = false;
	bool		listed = false;
	bool		newer = false;
	XrefListed *x;

	if (ref->len == 0)
		return;
	hash_key(&key, ref);
	x = self->listed ? hash_search(self->listed, &key, HASH_FIND, NULL) : NULL;
	for (int i = 0; i < vr->nfiles; i++)
	{
		CheckedFile *f = &vr->files[i];
		uint64		offset;
		uint32		len;

		if (!PROLLY_CALL(table_format(), find, f->t->tf, &key, &offset, &len))
			continue;
		/* (in its own generations: nothing to list) */
		if (f->lo <= self->hi && f->hi >= self->lo)
			return;
		if (f->hi < self->lo)
		{
			older = true;
			for (int j = 0; x != NULL && j < x->n; j++)
				listed |= (x->epochs[j] >= f->lo && x->epochs[j] <= f->hi);
		}
		else
			newer = true;
	}
	if (older && listed)
		return;
	if (older)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s of table file \"%s\" names chunk %s of an older generation, which its section doesn't list",
						prolly_hash_hex(vr->chunk), self->t->path,
						prolly_hash_hex(ref))));
	if (newer)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s of table file \"%s\" names chunk %s, which only a newer generation holds",
						prolly_hash_hex(vr->chunk), self->t->path,
						prolly_hash_hex(ref))));
	if (stored_has(vr->s, &key))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s of table file \"%s\" names chunk %s, which no settled file holds",
						prolly_hash_hex(vr->chunk), self->t->path,
						prolly_hash_hex(ref))));
}

/* One pass over the manifest's settled files; false if they changed. */
static bool
verify_pass(ProllyChunkStore *s, int64 *checked)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	uint64		before = manifests_count();
	Manifest	m;
	CheckedFile *files;
	int			n = 0;
	bool		whole = true;
	StringInfoData bytes;
	StringInfoData chunk;
	HASHCTL		ctl;

	(void) read_manifest(dir, &m);
	files = palloc0(sizeof(CheckedFile) * Max(m.nfiles, 1));
	ctl.keysize = sizeof(ProllyHash);
	ctl.hash = chunk_hash_key_hash;
	ctl.entrysize = sizeof(XrefListed);
	ctl.hcxt = CurrentMemoryContext;
	for (int i = 0; i < m.nfiles && whole; i++)
	{
		XrefSection x;

		if ((m.files[i].flags & MANIFEST_FILE_SETTLED) == 0)
			continue;
		files[n].t = open_table(dir, &m.files[i], CurrentMemoryContext);
		if (files[n].t == NULL)
		{
			whole = false;		/* (replaced since) */
			break;
		}
		files[n].lo = m.files[i].epoch_lo;
		files[n].hi = m.files[i].epoch_hi;
		if (xrefs_read(files[n].t, &x))
		{
			files[n].listed = hash_create("prolly generations listed", 64,
										  &ctl,
										  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
			xrefs_visit(&x, 0, PG_UINT32_MAX, note_listed, files[n].listed);
			xrefs_free(&x);
		}
		n++;
	}
	initStringInfo(&bytes);
	initStringInfo(&chunk);
	for (int i = 0; i < n && whole; i++)
	{
		const ProllyTableFileRoutine *fmt = table_format();
		VerifyRef	vr = {s, files, n, i, NULL};

		for (int e = 0; e < PROLLY_CALL(fmt, count, files[i].t->tf); e++)
		{
			ProllyHash	h;
			uint64		offset;
			uint32		len;

			CHECK_FOR_INTERRUPTS();
			PROLLY_CALL(fmt, entry, files[i].t->tf, e, &h, &offset, &len);
			resetStringInfo(&bytes);
			enlargeStringInfo(&bytes, len);
			if (!table_file_read(files[i].t, offset, bytes.data, len))
			{
				whole = false;
				break;
			}
			resetStringInfo(&chunk);
			PROLLY_CALL(fmt, decode, files[i].t->tf, bytes.data, len, &chunk);
			vr.chunk = &h;
			prolly_chunk_references(chunk.data, chunk.len, verify_ref, &vr);
			(*checked)++;
		}
	}
	for (int i = 0; i < n; i++)
	{
		close_table(files[i].t);
		if (files[i].listed)
			hash_destroy(files[i].listed);
	}
	pfree(files);
	pfree(bytes.data);
	pfree(chunk.data);
	pfree(dir);
	return whole && manifests_count() == before;
}

/*
 * Check the settled files' generations (see the callback's contract): a
 * pass again if a manifest replaced them meanwhile, a few times.
 */
static int64
journal_verify_generations(ProllyChunkStore *s)
{
	int64		checked = 0;

	for (int attempt = 0; attempt < 5; attempt++)
	{
		checked = 0;
		if (verify_pass(s, &checked))
			break;
	}
	return checked;
}

/*
 * The first generation of the settled file holding a chunk, the oldest if
 * several do; 0 if none does (the journal's, or a file not settled, or
 * not here).
 */
static uint32
journal_epoch_of(ProllyChunkStore *s, const ProllyHash *hash)
{
	ProllyHash	key;
	uint32		epoch = 0;
	ListCell   *lc;

	hash_key(&key, hash);
	refresh_segments(s);
	load_tables(s, false);
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);
		uint64		offset;
		uint32		len;

		if (t->settled && (epoch == 0 || t->epoch_lo < epoch) &&
			PROLLY_CALL(table_format(), find, t->tf, &key, &offset, &len))
			epoch = t->epoch_lo;
	}
	return epoch;
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
	jindex_append_lock(s->ix, true);
	/* after every append to it: each reserved its range under this lock */
	sealed[m->nsegments - 2] = GetXLogInsertRecPtr();
	segment_header(s->dboid, s->spcoid, id);
	INJECTION_POINT("prolly-segment-made", NULL);
	replace_manifest(s, m, XLOG_CHUNKS_SEAL);
	jindex_append_unlock(s->ix);
	return id;
}

static void
journal_seal(ProllyChunkStore *s)
{
	Manifest	m;

	lock_manifest(s, &m);
	(void) seal_segment(s, &m);
}

/* journal_segments()'s count: each chunk once per segment it is in */
typedef struct SegmentCounts
{
	ProllyChunkStore *s;
	int			nsegs;
	int		   *counts;
	int		   *last;			/* the ordinal of the chunk last counted */
} SegmentCounts;

static void
count_copy(const ProllyHash *hash, uint64 segment, int ordinal, void *arg)
{
	SegmentCounts *sc = arg;
	ListCell   *lc;
	int			i = 0;

	foreach(lc, sc->s->segments)
	{
		if (((Segment *) lfirst(lc))->id == segment)
			break;
		i++;
	}
	if (i < sc->nsegs && sc->last[i] != ordinal)
	{
		sc->counts[i]++;
		sc->last[i] = ordinal;
	}
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
		SegmentCounts sc = {s, nsegs, counts, last};

		jindex_visit(s->ix, count_copy, &sc);
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
 * retires the segments it collected, and their ROOT records with them;
 * the root they ended at is the manifest's base, given as *before.
 *
 * Only each record's length and kind are read (through a buffer) but for
 * the ROOT records, read whole and checked: the chunks between are passed
 * over unread.  A ROOT record that doesn't check is one being written over
 * its PAD (of the same length) as it was read: passed over too, as the
 * PAD would be.
 */
#define ROOTS_BUFFER	(64 * 1024)
static void
journal_roots(ProllyChunkStore *s, ProllyHash *before,
			  ProllyChunkRootVisit visit, void *arg)
{
	char	   *buf = palloc(ROOTS_BUFFER);
	char	   *rec = NULL;
	Size		reclen = 0;

	/*
	 * The base as of the manifest the shared index's segments are: a
	 * collection between this backend's read of its manifest and the lock
	 * would pair the segments it left with the base before it.  (Each swap
	 * names a new segment, so the same segments are the same base.)
	 */
	for (int tries = 0;; tries++)
	{
		bool		same;

		scan_journal(s);
		jindex_lock(s->ix, false);
		same = jindex_scan(s->ix)->nsegments == list_length(s->segments);
		for (int i = 0; same && i < list_length(s->segments); i++)
			same = jindex_segment(s->ix, i)->id ==
				((Segment *) list_nth(s->segments, i))->id;
		if (same || tries == 3)
		{
			*before = same ? s->base : (ProllyHash) {0};
			break;
		}
		jindex_unlock(s->ix);
	}
	for (int si = 0; si < jindex_scan(s->ix)->nsegments; si++)
	{
		JournalSegScan *sg = jindex_segment(s->ix, si);
		JournalHole *holes = jindex_holes(s->ix, sg);
		Segment    *g = local_segment(s, sg->id);
		uint64		offset = JOURNAL_HEADER_SIZE;
		uint64		buf_at = 0;
		Size		buf_len = 0;

		if (!open_segment(g))
			continue;
		while (offset + RECORD_OVERHEAD <= sg->scanned)
		{
			bool		skipped = false;
			uint32		len;
			uint8		kind;

			CHECK_FOR_INTERRUPTS();
			for (int h = 0; h < sg->nholes; h++)
			{
				if (holes[h].offset == offset)
				{
					offset += holes[h].len;
					skipped = true;
					break;
				}
			}
			if (skipped)
				continue;
			if (offset < buf_at || offset + sizeof(uint32) + 1 > buf_at + buf_len)
			{
				buf_at = offset;
				buf_len = Min(ROOTS_BUFFER, sg->scanned - offset);
				read_exact(g, buf, buf_len, buf_at);
			}
			memcpy(&len, buf + (offset - buf_at), sizeof(len));
			kind = (uint8) buf[offset - buf_at + sizeof(uint32)];
			if (len < RECORD_OVERHEAD || offset + len > sg->scanned)
				break;
			if (kind == KIND_ROOT)
			{
				ProllyHash	h;
				Size		after;

				if (len > reclen)
				{
					reclen = Max(len, 256);
					rec = rec ? repalloc(rec, reclen) : palloc(reclen);
				}
				read_exact(g, rec, len, offset);
				if (record_valid(rec, len) && record_hash(rec, len, &h, &after))
					visit(&h, record_root_time(rec, len, after), arg);
			}
			offset += len;
		}
	}
	jindex_unlock(s->ix);
	pfree(buf);
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
	uint32		epoch_lo;		/* the generations its chunks copied are */
	uint32		epoch_hi;		/* written in (0: not settled) */
	int			group;			/* in gc->groups: the files written so */
	HTAB	   *listed;			/* what its section lists (listed_epoch()) */
	bool		repaired;		/* MANIFEST_FILE_REPAIRED */
} GcFile;

/*
 * The collected files whose chunks are copied to files of the same
 * generations: a settled file's own (a collection keeps each chunk's
 * generation), or the collection's, for what it settles (0: none).
 */
typedef struct GcGroup
{
	uint32		epoch_lo;
	uint32		epoch_hi;
	uint64	   *order;			/* its slots, file by file in offset order */
	uint64		n;
	bool		repaired;		/* a file of it is (so are those written) */
} GcGroup;

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
	ProllyHash	sealed_root;	/* the root the collected segments end at,
								 * the base of the moves kept (len 0: not
								 * known) */
	int			nfiles;
	GcFile	   *files;
	uint64		nslots;
	HTAB	   *slots;			/* the sealed segments' chunks */
	uint8		hash_len;
	List	   *xids;			/* TransactionId: appended to the sealed
								 * segments, in progress at the seal */
	VirtualTransactionId *held; /* begun as a standby's, holding pointers */
	int			nheld;
	bool		waited;
	Segment    *appended;		/* the segment appended to from the seal */
	int			keep;			/* its KeepSet, -1 once let go */
	bool		keep_all;		/* it had none */
	int			nretained;		/* sealed segments not collected: a logical */
	uint64	   *retained;		/* slot may still decode what was appended */
	XLogRecPtr *retained_at;	/* to them (their sealed_at) */
	HTAB	   *retained_chunks;	/* their chunks (ProllyHash), as
									 * gc_retained() visited them */
	bool		names_retained; /* a chunk copied names one of them */
	int			kept_seen;		/* how many of it gc_kept() visited */
	uint64		next_id;		/* the next file id to give */
	uint8	   *copied;			/* slots gc_copy() wrote */
	uint64	   *read_order;		/* gc_read_order()'s, made once */
	int			ngroups;
	GcGroup    *groups;			/* gc_groups()', made once */
	int			nothers;		/* settled files not collected, opened */
	TableFile **others;
	uint32	   *others_lo;		/* (their generations) */
	uint32	   *others_hi;
	List	   *others_repaired;	/* (of others: those repaired) */
	uint32		retire_lo;		/* a retire's generations (0: none) */
	uint32		retire_hi;
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
			remove_store_file(dboid, spcoid, k == 1, before->retiring[i],
							  paths[k]);
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
gc_begin_scoped(ProllyChunkStore *s, ProllyGcScope scope, uint32 retire_lo,
				uint32 retire_hi, bool if_free, ProllyHash *root,
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
	 * for every transaction running at its swap, the second among them
	 * (with if_free, told so, nothing begun).  No conjoin or new table file
	 * until the swap (from then on a transfer older than the swap is
	 * refused, not waited for).
	 */
	{
		LOCKTAG		tag;

		SET_LOCKTAG_OBJECT(tag, s->dboid, GC_LOCK_CLASSID, 0, 0);
		if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		{
			if (if_free)
				return NULL;
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("another collection of this database's chunk store is running")));
		}
	}

	cxt = AllocSetContextCreate(TopTransactionContext, "prolly collection",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	gc = palloc0(sizeof(ProllyChunkGc));
	dir = GetDatabasePath(s->dboid, s->spcoid);
	gc->s = s;
	gc->cxt = cxt;
	gc->full = full;
	gc->retire_lo = retire_lo;
	gc->retire_hi = retire_hi;
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
	ctl.hash = chunk_hash_key_hash;
	ctl.entrysize = sizeof(GcSlot);
	ctl.hcxt = cxt;
	gc->slots = hash_create("prolly collection slots", 1024, &ctl,
							HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
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
		f->epoch_lo = f->epoch_hi = gc->settle ? gc->m.gc_generation + 1 : 0;
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

		/* (a retire's: whole generations, each in [retire_lo, retire_hi]) */
		bool		retired = scope == PROLLY_GC_RETIRE && settled &&
			gc->m.files[i].epoch_lo >= retire_lo &&
			gc->m.files[i].epoch_hi <= retire_hi;

		if (scope == PROLLY_GC_RETIRE && settled && !retired &&
			gc->m.files[i].epoch_hi >= retire_lo &&
			gc->m.files[i].epoch_lo <= retire_hi)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("table file %llu holds generations %u to %u, not all of them retired",
							(unsigned long long) gc->m.files[i].id,
							gc->m.files[i].epoch_lo, gc->m.files[i].epoch_hi)));
		if (settled && !full && !retired && scope != PROLLY_GC_JOURNAL)
		{
			/* (to tell the generation of what the chunks copied name) */
			TableFile  *t = open_table(dir, &gc->m.files[i], cxt);

			if (t == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("table file %llu of the manifest is missing",
								(unsigned long long) gc->m.files[i].id)));
			if (gc->others == NULL)
			{
				gc->others = palloc(sizeof(TableFile *) * gc->m.nfiles);
				gc->others_lo = palloc(sizeof(uint32) * gc->m.nfiles);
				gc->others_hi = palloc(sizeof(uint32) * gc->m.nfiles);
			}
			gc->others[gc->nothers] = t;
			if (gc->m.files[i].flags & MANIFEST_FILE_REPAIRED)
				gc->others_repaired = lappend_int(gc->others_repaired,
												  gc->nothers);
			gc->others_hi[gc->nothers] = gc->m.files[i].epoch_hi;
			gc->others_lo[gc->nothers++] = gc->m.files[i].epoch_lo;
			continue;
		}
		if ((settled && !full && !retired) || scope == PROLLY_GC_JOURNAL)
			continue;
		f = &gc->files[n++];
		f->id = gc->m.files[i].id;
		f->first = slot;
		f->settled = settled;
		f->repaired = (gc->m.files[i].flags & MANIFEST_FILE_REPAIRED) != 0;
		/* (a settled file's chunks keep their generations) */
		if (settled)
		{
			f->epoch_lo = gc->m.files[i].epoch_lo;
			f->epoch_hi = gc->m.files[i].epoch_hi;
		}
		else
			f->epoch_lo = f->epoch_hi = gc->settle ? gc->m.gc_generation + 1 : 0;
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
	gc->keep = keep_begin(s, &gc->xids, &gc->held, &gc->nheld);
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

static ProllyChunkGc *
journal_gc_begin(ProllyChunkStore *s, ProllyGcScope scope, bool if_free,
				 ProllyHash *root, uint64 *nslots)
{
	if (scope == PROLLY_GC_RETIRE)
		elog(ERROR, "a retire begins with gc_begin_retire()");
	return gc_begin_scoped(s, scope, 0, 0, if_free, root, nslots);
}

static ProllyChunkGc *
journal_gc_begin_retire(ProllyChunkStore *s, uint32 lo, uint32 hi,
						bool if_free, ProllyHash *root, uint64 *nslots)
{
	if (lo == 0 || lo > hi)
		elog(ERROR, "generations %u to %u can't be retired", lo, hi);
	return gc_begin_scoped(s, PROLLY_GC_RETIRE, lo, hi, if_free, root,
						   nslots);
}

static int64 journal_gc_find(ProllyChunkGc *gc, const ProllyHash *hash);
static void journal_gc_read(ProllyChunkGc *gc, uint64 slot, StringInfo out);

/*
 * A retire's: what the settled files it doesn't collect name in the
 * generations it does, as their sections list it (only a newer file's
 * can).
 */
/* gc_inbound()'s visit of what a repaired file's chunks name */
typedef struct RepairedNames
{
	ProllyChunkListedVisit visit;
	void	   *arg;
	ProllyHash	source;			/* the chunk naming them */
} RepairedNames;

static void
repaired_name(const ProllyHash *ref, void *arg)
{
	RepairedNames *rn = arg;

	/* (with the chunk naming it, so the horizon's cuts still cut) */
	if (ref->len > 0)
		rn->visit(ref, &rn->source, 0, rn->arg);
}

static void
journal_gc_inbound(ProllyChunkGc *gc, ProllyChunkListedVisit visit, void *arg)
{
	if (gc->retire_lo == 0)
		return;
	for (int i = 0; i < gc->nothers; i++)
	{
		XrefSection x;

		CHECK_FOR_INTERRUPTS();
		if (gc->others_hi[i] < gc->retire_lo || !xrefs_read(gc->others[i], &x))
			continue;
		xrefs_visit(&x, gc->retire_lo, gc->retire_hi, visit, arg);
		xrefs_free(&x);
	}

	/*
	 * A repaired file's chunks, every one: what a repair puts back in
	 * older generations is listed as it names newer ones, but a retire
	 * that freed a chunk one still names, were that ever missed, would
	 * lose history; so what they name is a root (epoch 0: any copy).
	 */
	foreach_int(i, gc->others_repaired)
	{
		TableFile  *t = gc->others[i];
		const ProllyTableFileRoutine *fmt = table_format();
		StringInfoData bytes;
		StringInfoData chunk;
		RepairedNames rn = {visit, arg, {0}};

		initStringInfo(&bytes);
		initStringInfo(&chunk);
		for (int e = 0; e < PROLLY_CALL(fmt, count, t->tf); e++)
		{
			ProllyHash	h;
			uint64		offset;
			uint32		len;

			CHECK_FOR_INTERRUPTS();
			PROLLY_CALL(fmt, entry, t->tf, e, &h, &offset, &len);
			resetStringInfo(&bytes);
			enlargeStringInfo(&bytes, len);
			if (!table_file_read(t, offset, bytes.data, len))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, t->path)));
			resetStringInfo(&chunk);
			PROLLY_CALL(fmt, decode, t->tf, bytes.data, len, &chunk);
			rn.source = h;
			prolly_chunk_references(chunk.data, chunk.len, repaired_name, &rn);
		}
		pfree(bytes.data);
		pfree(chunk.data);
	}
}

/*
 * A chunk, read as a collection reads: collected (gc_read()), or from a
 * settled file it doesn't collect; false if neither has it.
 */
static bool
journal_gc_read_any(ProllyChunkGc *gc, const ProllyHash *hash, StringInfo out)
{
	int64		slot = journal_gc_find(gc, hash);
	ProllyHash	key;

	if (slot >= 0)
	{
		journal_gc_read(gc, (uint64) slot, out);
		return true;
	}
	hash_key(&key, hash);
	for (int i = 0; i < gc->nothers; i++)
	{
		TableFile  *t = gc->others[i];
		uint64		offset;
		uint32		len;
		char	   *bytes;

		if (!PROLLY_CALL(table_format(), find, t->tf, &key, &offset, &len))
			continue;
		bytes = palloc(len);
		if (!table_file_read(t, offset, bytes, len))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
							len, (unsigned long long) offset, t->path)));
		PROLLY_CALL(table_format(), decode, t->tf, bytes, len, out);
		pfree(bytes);
		return true;
	}
	return false;
}

/*
 * The root the moves of the manifest's segments before upto end at: the
 * last ROOT record they hold, the newest segment's first, or else
 * fallback (none of them moved the root).  Asked once every move in flight
 * at the seal has ended, its ROOT (or its PAD, aborted) written in a
 * sealed segment: one in flight is newer than the root the seal saw.
 */
static void
segments_root(ProllyChunkGc *gc, int upto, const ProllyHash *fallback,
			  ProllyHash *root)
{
	char	   *rec = NULL;
	Size		reclen = 0;

	*root = *fallback;
	for (int i = upto - 1; i >= 0; i--)
	{
		Segment		g = {0};
		uint64		offset = JOURNAL_HEADER_SIZE;
		uint64		size;
		uint32		len;
		bool		found = false;

		g.id = gc->m.segments[i];
		g.path = segment_path(gc->s->dboid, gc->s->spcoid, g.id);
		g.file = -1;
		if (!open_segment(&g))
		{
			pfree(g.path);
			continue;
		}
		size = FileSize(g.file);
		while ((len = gc_record(&g, offset, size, &rec, &reclen)) > 0)
		{
			ProllyHash	h;
			Size		after;

			CHECK_FOR_INTERRUPTS();
			if ((uint8) rec[sizeof(uint32)] == KIND_ROOT &&
				record_hash(rec, len, &h, &after))
			{
				*root = h;
				found = true;
			}
			offset += len;
		}
		FileClose(g.file);
		pfree(g.path);
		if (found)
			break;
	}
	if (rec)
		pfree(rec);
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

	int64		oldest = -1;
	uint32		oldest_lo = 0;

	hash_key(&key, hash);

	/*
	 * A chunk in settled files collected (a full collection's) is the copy
	 * of the oldest generation, so that it keeps that one: what names it
	 * there may be as old.
	 */
	if (gc->full || gc->retire_lo > 0)
	{
		for (int i = 0; i < gc->nfiles; i++)
		{
			GcFile	   *f = &gc->files[i];
			int			at;

			if (f->segment || !f->settled ||
				(oldest >= 0 && f->epoch_lo >= oldest_lo))
				continue;
			at = PROLLY_CALL(table_format(), locate, f->t->tf, &key);
			if (at >= 0)
			{
				oldest = f->first + at;
				oldest_lo = f->epoch_lo;
			}
		}
		if (oldest >= 0)
			return oldest;
	}
	e = hash_search(gc->slots, &key, HASH_FIND, NULL);
	if (e != NULL)
		return e->slot;
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];
		int			at;

		if (f->segment || ((gc->full || gc->retire_lo > 0) && f->settled))
			continue;
		at = PROLLY_CALL(table_format(), locate, f->t->tf, &key);
		if (at >= 0)
			return f->first + at;
	}
	return -1;
}

static void
journal_gc_listed(ProllyChunkGc *gc, int file, ProllyChunkListedVisit visit,
				  void *arg)
{
	GcFile	   *f = &gc->files[file];
	XrefSection x;

	if (f->segment || f->t == NULL || !xrefs_read(f->t, &x))
		return;
	xrefs_visit(&x, 0, PG_UINT32_MAX, visit, arg);
	xrefs_free(&x);
}

static int64
journal_gc_find_in(ProllyChunkGc *gc, const ProllyHash *hash, uint32 epoch)
{
	ProllyHash	key;

	hash_key(&key, hash);
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];
		int			at;

		if (f->segment || !f->settled || epoch < f->epoch_lo ||
			epoch > f->epoch_hi)
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
 * decode from, or one of a standby's, as its feedback said (invalid: there
 * is none); no older than the WAL left (a collection keeps what that names).
 */
static XLogRecPtr
logical_horizon(Oid dboid)
{
	XLogRecPtr	wanted;

	return ReplicationSlotsLogicalDecodingHorizonKept(dboid, &wanted);
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

	if (gc->nretained > 0 && gc->retained_chunks == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.hash = chunk_hash_key_hash;
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = gc->cxt;
		gc->retained_chunks = hash_create("prolly collection retained chunks",
										  1024, &ctl,
										  HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}

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
				ProllyHash	key;

				hash_key(&key, &h);
				(void) hash_search(gc->retained_chunks, &key, HASH_ENTER, NULL);
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
		wait_for_readers(gc->held, gc->nheld);
		gc->waited = true;
		if (prepared != NIL)
			gc_prepared(gc, prepared, visit, arg);
		list_free(prepared);

		/*
		 * The root a reflog's moves begin at after the swap, kept whole.
		 * With every sealed segment collected, the root they end at (as of
		 * the seal, or one a move in flight then moved to).  With the
		 * newest sealed ones kept for a logical slot, their moves are kept
		 * too, so the root the collected ones before them end at (else the
		 * old manifest's base: no move since it).  Kept sealed segments
		 * that aren't the newest (one sealed when the WAL position wasn't
		 * known) leave it unknown.
		 */
		{
			int			upto = gc->m.nsegments - 1;
			ProllyHash	fallback = gc->m.root;

			if (gc->nretained > 0)
			{
				int			first = upto - gc->nretained;

				for (int k = 0; k < gc->nretained && upto >= 0; k++)
					if (first < 0 || gc->m.segments[first + k] != gc->retained[k])
						upto = -1;
				if (upto >= 0)
				{
					upto = first;
					fallback = gc->m.base;
				}
			}
			memset(&gc->sealed_root, 0, sizeof(gc->sealed_root));
			if (upto >= 0)
				segments_root(gc, upto, &fallback, &gc->sealed_root);
		}
		if (gc->sealed_root.len > 0)
			visit(&gc->sealed_root, NULL, 0, arg);
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
		jindex_append_lock(s->ix, true);
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
	uint64	   *order;			/* the slots, file by file in offset order */
	uint64		norder;
	GcFile	   *src;			/* the file of the slot given last */
	/* the window of a file last read: its bytes [start, start + buf.len) */
	GcFile	   *window_file;
	uint64		window_start;
	StringInfoData window;
	CopyFrom	from;			/* where the slot given last is */
	StringInfoData chunk;		/* and its chunk, decoded to check it */
} GcCopy;

/* a copy reads its files in windows of this much */
#define GC_WINDOW			(256 * 1024)

/* (a slot and where its bytes are, to order a file's by offset) */
typedef struct SlotAt
{
	uint64		slot;
	uint64		offset;
} SlotAt;

static int
slot_at_cmp(const void *a, const void *b)
{
	const SlotAt *x = a;
	const SlotAt *y = b;

	return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

/*
 * The collection's slots in the order a copy reads them: file by file,
 * each in the order its bytes are in (a segment's are already; a table
 * file's are numbered in hash order).
 */
static uint64 *
gc_read_order(ProllyChunkGc *gc)
{
	uint64	   *order = MemoryContextAllocHuge(gc->cxt,
											   sizeof(uint64) * Max(gc->nslots, 1));
	uint64		n = 0;

	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];

		if (f->segment || f->t == NULL)
		{
			for (uint64 j = 0; j < f->count; j++)
				order[n++] = f->first + j;
			continue;
		}
		{
			SlotAt	   *at = MemoryContextAllocHuge(CurrentMemoryContext,
													sizeof(SlotAt) * Max(f->count, 1));

			for (uint64 j = 0; j < f->count; j++)
			{
				ProllyHash	h;
				uint32		len;

				PROLLY_CALL(table_format(), entry, f->t->tf, (int) j, &h,
							&at[j].offset, &len);
				at[j].slot = f->first + j;
			}
			qsort(at, f->count, sizeof(SlotAt), slot_at_cmp);
			for (uint64 j = 0; j < f->count; j++)
				order[n++] = at[j].slot;
			pfree(at);
		}
	}
	Assert(n == gc->nslots);
	return order;
}

/*
 * A slot's chunk, framed as stored, read through the copy's window (a
 * file's next bytes are read GC_WINDOW at a time, as the copy reads them
 * in order); gcc->from says where it is, and gcc->chunk gets it decoded.
 */
static void
gc_read_windowed(GcCopy *gcc, uint64 slot, StringInfo out)
{
	ProllyChunkGc *gc = gcc->gc;
	GcFile	   *f = slot_file(gc, slot);
	uint64		i = slot - f->first;
	uint64		offset;
	uint32		len;
	const char *bytes;

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
	if (gcc->window_file != f || offset < gcc->window_start ||
		offset + len > gcc->window_start + gcc->window.len)
	{
		uint64		want = Max((uint64) len, GC_WINDOW);
		uint64		size = f->size;
		ssize_t		got;

		if (size > offset && size - offset < want)
			want = Max(size - offset, (uint64) len);
		resetStringInfo(&gcc->window);
		enlargeStringInfo(&gcc->window, (int) want);
		got = FileRead(f->segment ? f->file : f->t->file, gcc->window.data, want,
					   offset, WAIT_EVENT_DATA_FILE_READ);
		if (got < (ssize_t) len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
							len, (unsigned long long) offset, f->path)));
		pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_FILE_READS, 1);
		/* (what was read: a short read holds less than was asked for) */
		gcc->window.len = (int) got;
		gcc->window_file = f;
		gcc->window_start = offset;
	}
	bytes = gcc->window.data + (offset - gcc->window_start);
	resetStringInfo(&gcc->chunk);
	if (f->segment)
	{
		record_framed(f->path, bytes, len, out);
		chunk_bytes(f->path, bytes, len, &gcc->chunk);
	}
	else
	{
		prolly_tablefile_read_stored(table_format(), f->t->tf, bytes, len, out);
		PROLLY_CALL(table_format(), decode, f->t->tf, bytes, len, &gcc->chunk);
	}
	gcc->from.src.kind = f->segment ? COPY_SEGMENT_CHUNK : COPY_TABLE_BYTES;
	gcc->from.src.id = f->id;
	gcc->from.src.offset = offset;
	gcc->from.src.len = len;
	gcc->from.bytes = bytes;
}

/* Does the collected slot (of file f) hold the chunk hash? */
static bool
slot_holds(ProllyChunkGc *gc, GcFile *f, uint64 slot, const ProllyHash *hash)
{
	ProllyHash	key;

	hash_key(&key, hash);
	if (f->segment)
	{
		GcSlot	   *e = hash_search(gc->slots, &key, HASH_FIND, NULL);

		return e != NULL && e->slot == slot;
	}
	return PROLLY_CALL(table_format(), locate, f->t->tf, &key) ==
		(int) (slot - f->first);
}

/*
 * A chunk a collection copies names ref: one of the segments it keeps as
 * they are, for a logical slot, has it?  Then what it writes isn't
 * settled: a later collection, of those segments too, must still find
 * what the chunks it writes name.
 */
static void
note_retained(const ProllyHash *ref, void *arg)
{
	ProllyChunkGc *gc = arg;
	ProllyHash	key;

	if (gc->names_retained || ref->len == 0)
		return;
	hash_key(&key, ref);
	if (hash_search(gc->retained_chunks, &key, HASH_FIND, NULL) != NULL)
		gc->names_retained = true;
}

static bool
gc_copy_next(void *arg, ProllyHash *hash, StringInfo data)
{
	GcCopy	   *gcc = arg;
	ProllyChunkGc *gc = gcc->gc;

	while (gcc->next < gcc->norder)
	{
		uint64		slot = gcc->order[gcc->next++];
		GcFile	   *f;
		ProllyHashCtx *ctx;

		if (!marked(gcc->marks, slot) || marked(gc->copied, slot))
			continue;
		f = slot_file(gc, slot);
		if (gcc->keep[f - gc->files])
			continue;
		gcc->src = f->segment ? NULL : f;
		gc->copied[slot / 8] |= 1 << (slot % 8);
		gc_read_windowed(gcc, slot, data);

		/* (read and written: a page missed and one dirtied) */
		if (gcc->paced)
		{
			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageMiss + VacuumCostPageDirty;
			vacuum_delay_point(false);
		}

		/* checked as it is copied (as stored): its bytes are its hash's */
		ctx = PROLLY_CALL(gcc->hash, begin, CurrentMemoryContext);
		PROLLY_CALL(gcc->hash, update, ctx, (const uint8 *) gcc->chunk.data,
					gcc->chunk.len);
		PROLLY_CALL(gcc->hash, finish, ctx, hash);
		if (!slot_holds(gc, f, slot, hash))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("chunk in file \"%s\" doesn't match its hash",
							f->path)));
		if (gc->retained_chunks != NULL && !gc->names_retained)
			prolly_chunk_references(gcc->chunk.data, gcc->chunk.len,
									note_retained, gc);
		return true;
	}
	return false;
}

/*
 * Write the marked slots not yet written (but kept files') to new files,
 * each of them begun once the last holds prolly.gc_file_size of chunks (as
 * Dolt's collections rotate theirs), so that a store's files are of about
 * one size, which a remote can be sent whole; paced by the cost-based
 * vacuum delay where one is active (not in the window, where interrupts
 * are held).
 */
/*
 * The generation a chunk copied names is in, as the collection leaves it:
 * a file kept whole keeps its own; a chunk copied goes to its group's; one
 * not marked isn't kept (0); one not collected is in a settled file's.
 */
static uint32
gc_resolve(void *arg, const ProllyHash *target, bool *always)
{
	GcCopy	   *gcc = arg;
	ProllyChunkGc *gc = gcc->gc;
	int64		slot = journal_gc_find(gc, target);
	ProllyHash	key;

	GcFile	   *f = slot >= 0 ? slot_file(gc, (uint64) slot) : NULL;

	if (f != NULL &&
		(gcc->keep[f - gc->files] || marked(gcc->marks, (uint64) slot)))
		return f->epoch_lo;
	hash_key(&key, target);
	for (int i = 0; i < gc->nothers; i++)
	{
		uint64		offset;
		uint32		len;

		if (PROLLY_CALL(table_format(), find, gc->others[i]->tf, &key,
						&offset, &len))
			return gc->others_lo[i];
	}

	/*
	 * Collected, not marked, and named by a chunk marked: a parent the
	 * history horizon cuts, dropped now.  Listed by its generation all the
	 * same, so a repair puts it back there, and a retire finds it named.
	 */
	if (f != NULL)
	{
		*always = true;
		return f->epoch_lo;
	}
	/* (gone: what the file the chunk comes from listed it as, if it did) */
	return gcc->src ? listed_epoch(gcc->src->t, &gcc->src->listed, gc->cxt,
								   &key, always) : 0;
}

/* The groups of the files collected, made once, oldest first. */
static void
gc_groups(ProllyChunkGc *gc)
{
	uint64	   *counts;

	if (gc->groups != NULL)
		return;
	if (gc->read_order == NULL)
		gc->read_order = gc_read_order(gc);
	gc->groups = MemoryContextAllocZero(gc->cxt,
										sizeof(GcGroup) * Max(gc->nfiles, 1));
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];
		int			g;

		for (g = 0; g < gc->ngroups; g++)
		{
			if (gc->groups[g].epoch_lo == f->epoch_lo &&
				gc->groups[g].epoch_hi == f->epoch_hi)
				break;
		}
		if (g == gc->ngroups)
		{
			/* (in order: older generations first) */
			while (g > 0 && gc->groups[g - 1].epoch_lo > f->epoch_lo)
			{
				gc->groups[g] = gc->groups[g - 1];
				g--;
			}
			gc->groups[g].epoch_lo = f->epoch_lo;
			gc->groups[g].epoch_hi = f->epoch_hi;
			gc->groups[g].n = 0;
			gc->ngroups++;
			for (int j = 0; j < i; j++)
			{
				if (gc->files[j].group >= g)
					gc->files[j].group++;
			}
		}
		f->group = g;
	}
	for (int i = 0; i < gc->nfiles; i++)
		gc->groups[gc->files[i].group].repaired |= gc->files[i].repaired;
	counts = palloc0(sizeof(uint64) * Max(gc->ngroups, 1));
	for (int i = 0; i < gc->nfiles; i++)
		counts[gc->files[i].group] += gc->files[i].count;
	for (int g = 0; g < gc->ngroups; g++)
		gc->groups[g].order = MemoryContextAllocHuge(gc->cxt,
													 sizeof(uint64) * Max(counts[g], 1));
	for (uint64 i = 0; i < gc->nslots; i++)
	{
		uint64		slot = gc->read_order[i];
		GcGroup    *g = &gc->groups[slot_file(gc, slot)->group];

		g->order[g->n++] = slot;
	}
	pfree(counts);
}

/*
 * Write the marked slots not yet written (but kept files') to new files,
 * a group's to files of its generations, each of them begun once the last
 * holds prolly.gc_file_size of chunks (as Dolt's collections rotate
 * theirs), so that a store's files are of about one size, which a remote
 * can be sent whole; paced by the cost-based vacuum delay where one is
 * active (not in the window, where interrupts are held).  A settled file
 * lists in its section what its chunks name in older generations.
 */
static void
gc_write(ProllyChunkGc *gc, const uint8 *marks, const bool *keep, bool paced)
{
	GcCopy		gcc = {gc, marks, keep, 0, prolly_content_hash(), paced};
	XrefBuild	xb = {0};
	uint32		nchunks;
	uint64		size;
	bool		wrote;
	bool		full;

	if (gc->copied == NULL)
		gc->copied = MemoryContextAllocZero(gc->cxt, gc->nslots / 8 + 1);
	/* (once for the collection: its files and slots don't change) */
	gc_groups(gc);
	initStringInfo(&gcc.window);
	initStringInfo(&gcc.chunk);
	initStringInfo(&xb.chunk);
	xb.resolve = gc_resolve;
	xb.arg = &gcc;
	for (int g = 0; g < gc->ngroups; g++)
	{
		gcc.order = gc->groups[g].order;
		gcc.norder = gc->groups[g].n;
		gcc.next = 0;
		xb.lo = gc->groups[g].epoch_lo;
		xb.hi = gc->groups[g].epoch_hi;
		do
		{
			/*
			 * (not in the window, where appends wait: what came since is few
			 * chunks, written as one file, fsynced once; the chunks as stored,
			 * logged as copies)
			 */
			wrote = write_file(gc->s, gc->next_id, gc_copy_next, &gcc, true,
							   &gcc.from,
							   paced ? (uint64) prolly_gc_file_size * 1024 : 0,
							   xb.lo > 0 ? &xb : NULL,
							   &full, &nchunks, &size);
			if (wrote)
			{
				ManifestFile *f = MemoryContextAlloc(gc->cxt, sizeof(ManifestFile));
				MemoryContext old = MemoryContextSwitchTo(gc->cxt);

				pgstat_count_prolly(PGSTAT_PROLLY_GC_BYTES_COPIED, size);
				f->id = gc->next_id++;
				f->nchunks = nchunks;
				f->size = size;
				f->flags = xb.lo > 0 ? MANIFEST_FILE_SETTLED : 0;
				if (xb.lo > 0 && gc->groups[g].repaired)
					f->flags |= MANIFEST_FILE_REPAIRED;
				f->epoch_lo = xb.lo;
				f->epoch_hi = xb.hi;
				f->garbage = 0;
				gc->written = lappend(gc->written, f);
				MemoryContextSwitchTo(old);
			}
		} while (wrote && full);
	}
	pfree(gcc.window.data);
	pfree(gcc.chunk.data);
	pfree(xb.chunk.data);
	if (xb.items)
		pfree(xb.items);
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
 * The generations the new manifest nm (its files named) records: the
 * collection's own, if it settled chunks new to settled files, after
 * those of the old one still held by a settled file, or older than one
 * is.
 */
static void
swap_epochs(ProllyChunkGc *gc, Manifest *nm)
{
	uint32		oldest = PG_UINT32_MAX;
	bool		settled_new = false;
	ListCell   *lc;

	foreach(lc, gc->written)
	{
		ManifestFile *f = lfirst(lc);

		if (f->epoch_lo == nm->gc_generation)
			settled_new = true;
	}

	for (int i = 0; i < nm->nfiles; i++)
	{
		if (nm->files[i].epoch_lo > 0)
			oldest = Min(oldest, nm->files[i].epoch_lo);
	}
	nm->epochs = palloc(sizeof(ManifestEpoch) * (gc->m.nepochs + 1));
	nm->nepochs = 0;
	for (int i = 0; i < gc->m.nepochs; i++)
	{
		if (gc->m.epochs[i].epoch >= oldest)
			nm->epochs[nm->nepochs++] = gc->m.epochs[i];
	}
	if (settled_new)
	{
		nm->epochs[nm->nepochs].epoch = nm->gc_generation;
		nm->epochs[nm->nepochs].ended = GetCurrentTimestamp();
		nm->nepochs++;
	}
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

		/* (naming what a logical slot keeps the segments of: not settled) */
		if (gc->names_retained)
		{
			f->flags &= ~MANIFEST_FILE_SETTLED;
			f->epoch_lo = f->epoch_hi = 0;
		}
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

	/*
	 * What a reflog reads from: the moves the segments kept record, after
	 * the root the collected ones end at, which the collection kept whole
	 * (gc_new()'s; none if it couldn't tell).
	 */
	nm.base = gc->sealed_root;
	swap_epochs(gc, &nm);
	replace_manifest(s, &nm, XLOG_CHUNKS_SWAP);
	jindex_append_unlock(s->ix);
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

	{
		ProllyGcResult r = {0};

		for (uint64 slot = 0; slot < gc->nslots; slot++)
		{
			if (keep[slot_file(gc, slot) - gc->files])
				continue;
			if (marked(marks, slot))
				r.chunks_kept++;
			else
				r.chunks_freed++;
		}
		r.bytes_freed = freed_bytes > written_bytes ?
			freed_bytes - written_bytes : 0;
		r.window_usec = window_usec;
		/*
		 * (a move is a collection of the journal that keeps everything:
		 * counted as one alone, so a collection's totals are collections')
		 */
		if (gc->settle)
		{
			pgstat_count_prolly(PGSTAT_PROLLY_GC_COLLECTIONS, 1);
			pgstat_count_prolly(PGSTAT_PROLLY_GC_CHUNKS_KEPT, r.chunks_kept);
			pgstat_count_prolly(PGSTAT_PROLLY_GC_CHUNKS_FREED, r.chunks_freed);
			pgstat_count_prolly(PGSTAT_PROLLY_GC_BYTES_FREED, r.bytes_freed);
			pgstat_count_prolly(PGSTAT_PROLLY_GC_WINDOW_USEC, r.window_usec);
		}
		else
			pgstat_count_prolly(PGSTAT_PROLLY_GC_MOVES, 1);
		if (result)
			*result = r;
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
			release_table(gc->files[i].t);
	}
	for (int i = 0; i < gc->nothers; i++)
		release_table(gc->others[i]);
	gc->nothers = 0;
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
	pg_atomic_init_u32(&js->sync_all, 0);
	SpinLockInit(&js->mutex);
	ConditionVariableInit(&js->written);
	ConditionVariableInit(&js->swapped);
	js->nranges = append_ranges();
}

/*
 * The end of a segment's last whole record (fd, size bytes): what follows
 * is a range a crash left reserved, whose zeros the WAL never had, so a
 * standby may not have it.
 */
static uint64
records_end(int fd, uint64 size)
{
	uint64		offset = JOURNAL_HEADER_SIZE;
	StringInfoData rec;

	if (size < JOURNAL_HEADER_SIZE)
		return 0;
	initStringInfo(&rec);
	while (offset + RECORD_OVERHEAD <= size)
	{
		uint32		len;

		if (pg_pread(fd, &len, sizeof(len), offset) != sizeof(len) ||
			len < RECORD_OVERHEAD || offset + len > size)
			break;
		resetStringInfo(&rec);
		enlargeStringInfo(&rec, len);
		if (pg_pread(fd, rec.data, len, offset) != len ||
			!record_valid(rec.data, len))
			break;
		offset += len;
	}
	pfree(rec.data);
	return offset;
}

/*
 * Copy a file of the template's store (table file or segment id, spath,
 * size bytes) to dpath, the new database's, a piece at a time, each
 * WAL-logged as a copy of the template's bytes, not as the bytes (#564);
 * but a segment's bytes past its last whole record as bytes.  Synced, so
 * the copy is durable before the template can be dropped.
 */
static void
copy_store_file(Oid src_dboid, Oid src_spcoid, Oid dst_dboid, Oid dst_spcoid,
				bool segment, uint64 id, const char *spath, const char *dpath,
				uint64 size, char *buf)
{
	CopyRun		run = {src_dboid, src_spcoid, 0, 0, NULL};
	int			fd = OpenTransientFile(spath, O_RDONLY | PG_BINARY);
	int			dfd;
	uint64		end;

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", spath)));
	dfd = OpenTransientFile(dpath, O_RDWR | O_CREAT | (segment ? 0 : O_TRUNC) |
							PG_BINARY);
	if (dfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", dpath)));
	end = segment ? records_end(fd, size) : size;
	for (uint64 offset = 0; offset < size;)
	{
		Size		len = Min((uint64) APPEND_BATCH,
							  (offset < end ? end : size) - offset);

		if (pg_pread(fd, buf, len, offset) != len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
							len, (unsigned long long) offset, spath)));
		if (offset < end)
		{
			CopySource	src = {segment ? COPY_SEGMENT_BYTES : COPY_TABLE_BYTES,
			id, offset, (uint32) len};

			/* (the bytes logged instead while a backup runs: copy_write()) */
			run.n = 0;
			copy_add(&run, &src);
			copy_write(dst_dboid, dst_spcoid, segment, id, offset, buf, len,
					   &run, true, dfd, dpath);
		}
		else
			logged_write(dst_dboid, dst_spcoid, id, offset, buf, len, -1, dfd,
						 dpath);
		offset += len;
	}
	if (CloseTransientFile(fd) != 0 || CloseTransientFile(dfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", dpath)));
	fsync_fname(dpath, false);
	if (run.sources)
		pfree(run.sources);
}

/*
 * A new database's table files and manifest: those of the database it was
 * made from, each file copied (copy_store_file()), then the manifest naming
 * them.
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

		copy_store_file(src_dboid, src_spcoid, dst_dboid, dst_spcoid, false,
						m.files[i].id, spath, dpath, m.files[i].size, buf);
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
 * A new database's store: the files of the database it was made from,
 * WAL-logged as copies of them (#564), whatever the strategy: one whose
 * files were copied (the file-copy strategy) has them already, written
 * over with the same bytes, so its store's durability is its own WAL's.
 * Redo, standbys and base backups make them from their copies of the
 * template.  Nobody else uses the template meanwhile (CREATE DATABASE sees
 * to that), so its files hold still.
 */
static void
journal_copy_database(Oid src_dboid, Oid src_spcoid, Oid dst_dboid,
					  Oid dst_spcoid)
{
	char	   *sdir = GetDatabasePath(src_dboid, src_spcoid);
	struct stat st;
	char	   *buf;
	Manifest	m;

	copy_table_files(src_dboid, src_spcoid, dst_dboid, dst_spcoid);
	(void) read_manifest(sdir, &m);
	buf = palloc(APPEND_BATCH);
	for (int i = 0; i < m.nsegments; i++)
	{
		char	   *src = segment_path(src_dboid, src_spcoid, m.segments[i]);
		char	   *dst = segment_path(dst_dboid, dst_spcoid, m.segments[i]);

		if (stat(src, &st) == 0)
			copy_store_file(src_dboid, src_spcoid, dst_dboid, dst_spcoid, true,
							m.segments[i], src, dst, (uint64) st.st_size, buf);
		pfree(src);
		pfree(dst);
	}
	pfree(buf);
	pfree(sdir);
}

/*
 * The store's files that hold neither segments nor table files: those named
 * as the store's (its manifest, the journal's index, files received) but
 * not as either.
 */
static int64
journal_other_bytes(ProllyChunkStore *s)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	DIR		   *d = AllocateDir(dir);
	struct dirent *de;
	int64		bytes = 0;

	while ((de = ReadDir(d, dir)) != NULL)
	{
		char	   *path;
		struct stat st;

		if (strncmp(de->d_name, "prolly_", 7) != 0 ||
			strcmp(de->d_name, JOURNAL_FILE) == 0 ||
			strncmp(de->d_name, SEGMENT_PREFIX, strlen(SEGMENT_PREFIX)) == 0 ||
			strncmp(de->d_name, "prolly_table_", 13) == 0)
			continue;
		path = dir_file(dir, de->d_name);
		if (stat(path, &st) == 0)
			bytes += st.st_size;	/* (else removed meanwhile) */
		pfree(path);
	}
	FreeDir(d);
	pfree(dir);
	return bytes;
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
	.get_many = journal_get_many,
	.root = journal_root,
	.move_root = journal_move_root,
	.commit_payload = journal_commit_payload,
	.commit_writes = journal_commit_writes,
	.committed = journal_committed,
	.commit_redo = journal_commit_redo,
	.checkpoint = journal_checkpoint,
	.recovered = journal_recovered,
	.redo = journal_redo,
	.add_table_file = journal_add_table_file,
	.get_stored = journal_get_stored,
	.add_stored_table_file = journal_add_stored_table_file,
	.file_chunks = journal_file_chunks,
	.receive_file = journal_receive_file,
	.received_file = journal_received_file,
	.received_get = journal_received_get,
	.received_chunks = journal_received_chunks,
	.adopt_received = journal_adopt_received,
	.forget_received = journal_forget_received,
	.conjoin = journal_conjoin,
	.table_files = journal_table_files,
	.generations = journal_generations,
	.epochs = journal_epochs,
	.settle = journal_settle,
	.verify_generations = journal_verify_generations,
	.epoch_of = journal_epoch_of,
	.gc_listed = journal_gc_listed,
	.gc_find_in = journal_gc_find_in,
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
	.gc_begin_retire = journal_gc_begin_retire,
	.gc_inbound = journal_gc_inbound,
	.gc_read_any = journal_gc_read_any,
	.drop_database = journal_drop_database,
	.sync_file = journal_sync_file,
	.roots = journal_roots,
	.redo_consistent = journal_redo_consistent,
	.other_bytes = journal_other_bytes,
	.try_hold_root = journal_try_hold_root,
	.root_free = journal_root_free,
	.wait_root_free = journal_wait_root_free,
	.generation = journal_generation,
};
