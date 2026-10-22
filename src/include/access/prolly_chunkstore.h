/*-------------------------------------------------------------------------
 *
 * prolly_chunkstore.h
 *	  Chunk store interface: a database's history, as content-addressed
 *	  chunks and a root.
 *
 * A chunk is an immutable byte string named by its content hash (computed
 * by the caller, with a content-hash implementation).  A database's store
 * keeps its chunks and its root, the address of the chunk everything else
 * is found from, in files of its own, outside the buffer manager.
 *
 * Contract:
 * - put() adds a chunk with the calling transaction.  It is readable by
 *   the transaction at once, and by others once the transaction commits;
 *   a chunk of a transaction that aborts may stay, unreachable, until
 *   reclamation.  Putting a chunk the store has is allowed (the store may
 *   keep one copy or several).  A store that has a chunk has every chunk it
 *   names (prolly_chunk_references()): what a transaction puts is checked
 *   when it reaches the store's files (in batches, and the rest before the
 *   transaction commits), each chunk it names found among them or in the
 *   store, or the transaction fails with a serialization failure (the
 *   chunk named was collected after the transaction read it); a
 *   subtransaction that aborts takes the chunks it put with it.  Every
 *   change to the store's files is WAL-logged, so recovery, standbys and
 *   base backups carry the history, and a database made from another
 *   (CREATE DATABASE) gets a copy of it.
 * - get() appends a chunk's bytes to out; false if the store has none by
 *   that hash.  has() answers without reading the chunk.  get_many() gets
 *   several, visiting each (data NULL if the store has none), in any
 *   order: its reads of the store's files are made in offset order, those
 *   close together as one.
 * - root() is the root as last committed (or as moved by the calling
 *   transaction); an empty hash if it never was.
 * - move_root() moves the root with the calling transaction, if it is still
 *   expected (what the caller built on): false, without moving, if another
 *   transaction moved it since.  It waits for other movers, and they for
 *   it, until its transaction ends; the move is made durable by the
 *   transaction's commit record, and is gone if the transaction aborts.
 * - A transaction can't be prepared (two-phase) after moving a root.
 * - get_stored() appends a chunk as the store holds it, framed
 *   (prolly_chunk_frame()'s), without decoding it; add_stored_table_file()
 *   is add_table_file() of chunks next() gives framed so, written as they
 *   are (both optional: prolly_chunk_get_stored() and
 *   prolly_chunk_add_stored() decode and encode them otherwise).  So a
 *   transfer moves chunks as stored, compressed once, where they were made.
 * - file_chunks() visits the chunks of one of the store's files as stored,
 *   in its order: a table file table_files() names, or (name NULL) the
 *   journal, each of its chunks once.  A clone's sender reads them so.
 * - A clone's files (optional, the rest with receive_file()): the files a
 *   remote sends whole, each under its key.  receive_file() writes the
 *   chunks next() gives (framed) as the file for key, synced and WAL-logged
 *   as a table file is, but named by no manifest, so nothing reads it; it
 *   stays, through a failure too, until adopted or forgotten, so a clone
 *   tried again receives only what it lacks.  received_file() says
 *   whether the file for key is there whole (and its chunk count).
 *   received_get() reads a chunk of the files received (out NULL: whether
 *   they have it), received_chunks() visits each of their chunks, as
 *   stored.  adopt_received() names the files received in the manifest, at
 *   once; forget_received() removes them, but those for the nkeep keys of
 *   keep.
 * - add_table_file() adds a table file of the chunks next() gives (until it
 *   returns false), at once and for every transaction: the chunks of
 *   another store, received whole, or of a rewrite.  It isn't undone if the
 *   transaction aborts; its chunks are reachable once a root naming them
 *   is.  conjoin() rewrites table files as one, dropping chunks more than
 *   one of them holds: with max_files 0, all of them (settled if they all
 *   were); otherwise, if there are more than max_files, the smallest, as
 *   Dolt's conjoiner takes them (while those taken hold more chunks than
 *   the next, or leaving it would leave more than max_files), but no more
 *   in one pass than maintenance_work_mem and a gigabyte allow, settled
 *   files and the others apart.  Its copying is paced by the cost-based
 *   delay, where that is on.  table_files() visits the store's table
 *   files: a settled one is closed under what its chunks name (they are in
 *   it, in another settled file, or in the journal's segments).
 * - The journal (where put() appends) is in segments: seal() closes the one
 *   appends go to, and they go on to a new one; segments() visits them,
 *   oldest first (with count, each with its chunks counted; without, only
 *   whether it has any, nchunks 0 or -1, so nothing is read but sizes).
 * - other_bytes() is the bytes of the store's files that are neither
 *   segments nor table files (its manifest, an index of the journal, files
 *   received but not yet adopted).  Optional: without it, they are
 *   unknown.
 * - Garbage collection is the store's side of a collector's work
 *   (prolly_reclaim.h), in this order, in one transaction:
 *   gc_begin() holds off conjoins and new table files until the
 *   transaction ends (another collection meanwhile is refused, as is one
 *   in a transaction that used the store), seals the journal, and numbers
 *   the chunks collected (slots 0 .. *nslots - 1): the sealed segments',
 *   and the table files' (as scope says: those not settled, all of them,
 *   or none),
 *   each file's slots together, as gc_files() visits them; *root is the
 *   root as of the seal.  gc_find() gives a chunk's slot (-1: not collected), gc_read() a
 *   slot's bytes.  gc_new() visits what is new since the seal, or since its
 *   last call: the root, and each chunk appended since; the first call waits
 *   for the transactions that appended to the sealed segments and were in
 *   progress at the seal, and with window true it holds every append off
 *   until gc_swap().  gc_copy() writes the marked slots (but those of files
 *   kept whole) to new table files (settled, unless the scope is the
 *   journal), before the window; gc_swap()
 *   writes those marked since, and replaces the manifest: the new files and
 *   those kept, the segment appends go to, the next GC generation.  What
 *   was collected and not kept is no longer found; gc_retire() removes it
 *   once every transaction running at the swap has ended.  A transaction
 *   running at a swap that then appends a chunk naming one no longer found
 *   fails (a serialization failure), as does one adding a table file
 *   (add_table_file()) whose transaction began before the swap.
 * - keep() is told of a chunk a transaction names from outside the store
 *   (a hash pointer it writes into a table) without appending it: a
 *   collection keeps it if it runs then, or begins before the transaction
 *   ends or is prepared (its pointers are on pages then).  So gc_begin()
 *   makes the first gc_new() call wait for the transactions that told
 *   keep() of a chunk before the collection began, too, and gc_kept()
 *   visits the chunks told of since it began (false: too many to
 *   remember, and the collector must keep every chunk collected).  keep()
 *   fails (a serialization failure) if a swap dropped the chunk before it
 *   was told.
 * - A collection never waits for a prepared transaction, which may stay
 *   for days: gc_new() keeps as roots the chunks one appended to the
 *   segments collected, and gc_retire() doesn't wait for one running at
 *   the swap (it reads nothing more).
 *   gc_retained() visits the chunks a collection keeps whatever names
 *   them, the store's own reasons (a logical replication slot may still
 *   decode a row naming what was appended after its position): roots, as
 *   gc_new()'s.
 * - sync_file() fsyncs a file of a store a sync request names (one it
 *   made with SYNC_HANDLER_PROLLY_CHUNKS, its impl_id the tag's forknum),
 *   for a checkpoint: 0, or -1 with errno set and path the file's.  A file
 *   gone since (a collection retired it) is no error.
 * - shmem_size() and shmem_init() reserve and set up the shared state of
 *   the server's stores (optional).
 * - read_through() makes this backend's get() and has() ask source for a
 *   chunk the store hasn't, until it is cleared (source NULL) or the
 *   transaction ends: so walks that know what chunks hold can read another
 *   store's (a transfer's) as if it were this one's.  Its hint, if given
 *   (with or without a source), hears what prefetch() is told.
 * - roots() visits the roots the store was moved to that it still
 *   records, oldest first, with the time of each move: what a reflog is
 *   read from.  A collection may forget them.
 * - prefetch() is told the chunks a walk is about to read (a node's
 *   children), so that a read-through source can fetch them at once, or
 *   the store read them together (optional): whether it heard them, and
 *   with none (n 0), whether it would, so a walk collects them only when
 *   it would.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_chunkstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNKSTORE_H
#define PROLLY_CHUNKSTORE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"

typedef struct ProllyChunkStore ProllyChunkStore;
struct FileTag;

/* Visits a chunk another chunk names. */
typedef void (*ProllyChunkRefVisit) (const ProllyHash *ref, void *arg);

/* Visits a chunk as stored (framed). */
typedef void (*ProllyChunkStoredVisit) (const ProllyHash *hash,
										const char *frame, Size len,
										void *arg);

/* The next chunk to add, in hash and data; false when there are no more. */
typedef bool (*ProllyChunkNext) (void *arg, ProllyHash *hash, StringInfo data);

/*
 * Reads a chunk from elsewhere into out (NULL: only whether it has it);
 * false if it has none by that hash.
 */
typedef bool (*ProllyChunkGet) (void *arg, const ProllyHash *hash, StringInfo out);

/*
 * Visits a root the store was moved to, and when (0 if not known).
 */
typedef void (*ProllyChunkRootVisit) (const ProllyHash *root, TimestampTz at,
									  void *arg);

/* Visits chunk i of a get_many(): its bytes (NULL, 0: the store has none). */
typedef void (*ProllyChunkManyVisit) (int i, const char *data, Size len,
									  void *arg);

/* Hears the chunks a walk is about to read (prefetch()'s). */
typedef void (*ProllyChunkHint) (void *arg, const ProllyHash *hashes, int n);

typedef struct ProllyChunkGc ProllyChunkGc;

/* What one collection did. */
typedef struct ProllyGcResult
{
	uint64		chunks_kept;
	uint64		chunks_freed;
	uint64		bytes_freed;
	uint64		window_usec;	/* how long appends were held off */
} ProllyGcResult;

/*
 * Visits a chunk new since a collection's seal: its hash and bytes, or a
 * root (data NULL).
 */
typedef void (*ProllyChunkGcVisit) (const ProllyHash *hash, const char *data,
									Size len, void *arg);

/*
 * Visits a file a collection collects: its number, its slots (first, and
 * how many), whether it is settled, and its size in bytes.
 */
typedef void (*ProllyChunkGcFileVisit) (int file, uint64 first, uint64 nslots,
										bool settled, uint64 size, void *arg);

/*
 * What a collection collects: by default the journal's sealed segments and
 * the table files not settled; full, every file; the journal, its sealed
 * segments only.  What a collection of the journal writes isn't settled
 * (it may be told to keep what nothing names: a move).
 */
typedef enum ProllyGcScope
{
	PROLLY_GC_DEFAULT,
	PROLLY_GC_FULL,
	PROLLY_GC_JOURNAL,
} ProllyGcScope;

/*
 * Visits a table file or segment: its name, its chunk count, its size in
 * bytes, and whether it is settled (a table file closed under what its
 * chunks name).
 */
typedef void (*ProllyChunkFileVisit) (const char *name, int nchunks,
									  uint64 size, bool settled, void *arg);

typedef struct ProllyChunkStoreRoutine
{
	ProllyImplInfo info;

	/*
	 * The store of the database dboid, whose files are in tablespace spcoid,
	 * made if it has none.  Stays open for the session.
	 */
	ProllyChunkStore *(*open) (Oid dboid, Oid spcoid);

	void		(*put) (ProllyChunkStore *store, const ProllyHash *hash,
						const void *data, Size len);
	bool		(*get) (ProllyChunkStore *store, const ProllyHash *hash,
						StringInfo out);
	bool		(*has) (ProllyChunkStore *store, const ProllyHash *hash);
	void		(*get_many) (ProllyChunkStore *store, const ProllyHash *hashes,
							 int n, ProllyChunkManyVisit visit, void *arg);

	void		(*root) (ProllyChunkStore *store, ProllyHash *root);
	bool		(*move_root) (ProllyChunkStore *store, const ProllyHash *expected,
							  const ProllyHash *root);

	/*
	 * Commit.  commit_writes() is called first, late in the commit, after
	 * everything else that may put chunks (and while an error still aborts
	 * the transaction as any): it readies what the transaction has left to
	 * write
	 * and returns whether it has bytes to write once its commit record is
	 * flushed (so it is flushed at once, whatever synchronous_commit
	 * says).  commit_payload() returns what the commit record carries for
	 * the transaction's root move (0 bytes: none); committed() is called
	 * once the commit record is flushed, inside the commit's critical
	 * section, if either; commit_redo() replays the payload.
	 */
	Size		(*commit_payload) (char **data);
	bool		(*commit_writes) (void);
	void		(*committed) (void);
	void		(*commit_redo) (const char *data, Size len);

	/* Make every change before a checkpoint's redo point durable. */
	void		(*checkpoint) (void);

	/*
	 * Once crash recovery is over, before anyone reads a store: remove what
	 * writers and collections a crash cut short left (optional).  WAL may
	 * be written.
	 */
	void		(*recovered) (void);

	/* Replay the store's own records. */
	void		(*redo) (XLogReaderState *record);

	/* Table files. */
	void		(*add_table_file) (ProllyChunkStore *store, ProllyChunkNext next,
								   void *arg);
	void		(*conjoin) (ProllyChunkStore *store, int max_files);
	void		(*table_files) (ProllyChunkStore *store,
								ProllyChunkFileVisit visit, void *arg);

	/* Shared state. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	/*
	 * Give a new database (CREATE DATABASE's, its files copied or not) the
	 * store of the one it was made from, WAL-logged as any change (as
	 * copies of the template's files, which redo copies again).
	 */
	void		(*copy_database) (Oid src_dboid, Oid src_spcoid,
								  Oid dst_dboid, Oid dst_spcoid);

	/* Chunks the store hasn't, read from source (NULL: no longer). */
	void		(*read_through) (ProllyChunkStore *store, ProllyChunkGet source,
								 ProllyChunkHint hint, void *arg);
	bool		(*prefetch) (ProllyChunkStore *store, const ProllyHash *hashes,
							 int n);

	/* The journal's segments. */
	void		(*seal) (ProllyChunkStore *store);
	void		(*segments) (ProllyChunkStore *store, bool count,
							 ProllyChunkFileVisit visit, void *arg);

	/* Garbage collection: marks are a bit a slot, slot 0 the low bit of 0. */
	ProllyChunkGc *(*gc_begin) (ProllyChunkStore *store, ProllyGcScope scope,
								ProllyHash *root, uint64 *nslots);
	void		(*gc_files) (ProllyChunkGc *gc, ProllyChunkGcFileVisit visit,
							 void *arg);
	int64		(*gc_find) (ProllyChunkGc *gc, const ProllyHash *hash);
	void		(*gc_read) (ProllyChunkGc *gc, uint64 slot, StringInfo out);
	void		(*gc_new) (ProllyChunkGc *gc, bool window,
						   ProllyChunkGcVisit visit, void *arg);
	void		(*gc_copy) (ProllyChunkGc *gc, const uint8 *marks,
							const bool *keep);
	void		(*gc_swap) (ProllyChunkGc *gc, const uint8 *marks,
							const bool *keep, ProllyGcResult *result);
	void		(*gc_retire) (ProllyChunkGc *gc);

	/* Chunks named from outside the store, kept by a collection. */
	void		(*keep) (ProllyChunkStore *store, const ProllyHash *hash);
	bool		(*gc_kept) (ProllyChunkGc *gc, ProllyChunkRefVisit visit,
							void *arg);

	/*
	 * The chunks a collection keeps for the store's own reasons, roots
	 * (with their bytes, as gc_new()'s).
	 */
	void		(*gc_retained) (ProllyChunkGc *gc, ProllyChunkGcVisit visit,
								void *arg);

	/*
	 * Forget what the store keeps of a dropped database outside its
	 * directory (optional).
	 */
	void		(*drop_database) (Oid dboid);

	/* Checkpoints: a file a sync request names, fsynced (optional). */
	int			(*sync_file) (const struct FileTag *ftag, char *path);

	/*
	 * The roots the store was moved to, oldest first, each with when (the
	 * reflog): those whose moves it still records, which a garbage
	 * collection may forget (optional).
	 */
	void		(*roots) (ProllyChunkStore *store, ProllyChunkRootVisit visit,
						  void *arg);

	/* Chunks as stored, framed (optional). */
	bool		(*get_stored) (ProllyChunkStore *store, const ProllyHash *hash,
							   StringInfo out);
	void		(*add_stored_table_file) (ProllyChunkStore *store,
										  ProllyChunkNext next, void *arg);

	/* Whole files: sent, and received (optional). */
	void		(*file_chunks) (ProllyChunkStore *store, const char *name,
								ProllyChunkStoredVisit visit, void *arg);
	void		(*receive_file) (ProllyChunkStore *store, const char *key,
								 ProllyChunkNext next, void *arg);
	bool		(*received_file) (ProllyChunkStore *store, const char *key,
								  uint32 *nchunks);
	bool		(*received_get) (ProllyChunkStore *store, const ProllyHash *hash,
								 StringInfo out);
	void		(*received_chunks) (ProllyChunkStore *store,
									ProllyChunkStoredVisit visit, void *arg);
	void		(*adopt_received) (ProllyChunkStore *store);
	void		(*forget_received) (ProllyChunkStore *store,
									const char *const *keep, int nkeep);

	/*
	 * Recovery reached a consistent state (or crash recovery ended): what
	 * redo left unresolved, waiting for a later record, is an error
	 * (optional).
	 */
	void		(*redo_consistent) (void);
	int64		(*other_bytes) (ProllyChunkStore *store);

	/*
	 * Optional: what move_root() takes to move the root (held to the
	 * transaction's end), if no one else holds it: false, nothing taken,
	 * if another does.  For a step that must never wait for the root: one
	 * that's only worth doing now, or that holds what others may wait for.
	 */
	bool		(*try_hold_root) (ProllyChunkStore *store);
} ProllyChunkStoreRoutine;

#define PROLLY_CHUNKSTORE_CALLBACK_NAMES \
	"open", "put", "get", "has", "get_many", "root", "move_root", "commit_payload", \
	"commit_writes", "committed", "commit_redo", "checkpoint", "recovered", \
	"redo", \
	"add_table_file", \
	"conjoin", "table_files", "shmem_size", "shmem_init", "copy_database", \
	"read_through", "prefetch", "seal", "segments", "gc_begin", "gc_files", \
	"gc_find", "gc_read", "gc_new", "gc_copy", "gc_swap", "gc_retire", \
	"keep", "gc_kept", "gc_retained", "drop_database", "sync_file", "roots", \
	"get_stored", "add_stored_table_file", "file_chunks", "receive_file", \
	"received_file", "received_get", "received_chunks", "adopt_received", \
	"forget_received", "redo_consistent", "other_bytes", "try_hold_root"
#define PROLLY_CHUNKSTORE_FIRST_CALLBACK open
#define PROLLY_CHUNKSTORE_LAST_CALLBACK try_hold_root

static inline const ProllyChunkStoreRoutine *
prolly_chunkstore_lookup(const char *name)
{
	return (const ProllyChunkStoreRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHUNK_STORE, name);
}

/*
 * The chunk store the server uses, and the hooks transaction commit,
 * checkpoints and redo call (prolly_chunkstore.c).
 */
extern const ProllyChunkStoreRoutine *prolly_chunk_store(void);
extern Size prolly_chunkstore_commit_payload(char **data);
extern bool prolly_chunkstore_commit_writes(void);
extern void prolly_chunkstore_committed(void);
extern void prolly_chunkstore_commit_redo(const char *data, Size len);
extern void prolly_chunkstore_checkpoint(void);
extern void prolly_chunkstore_consistent(void);
extern void prolly_chunkstore_recovered(void);

/* The sync handler of the stores' files (storage/sync.h). */
extern int	prolly_chunks_syncfiletag(const struct FileTag *ftag, char *path);
extern bool prolly_chunks_filetagmatches(const struct FileTag *ftag,
										 const struct FileTag *candidate);

/*
 * A chunk as stored, which stores, table files and remotes move without
 * decoding it: its codec (PROLLY_CODEC_*), its length (4 bytes,
 * little-endian), then its bytes as the codec left them.
 * prolly_chunk_frame() appends one of a chunk to out, compressed where that
 * saves; prolly_chunk_unframe() appends the chunk a frame holds, false if
 * the bytes aren't one (a codec this server was built without is an
 * error).  prolly_chunk_frame_len() is the chunk's length a frame gives
 * (false if it isn't one).
 */
#define PROLLY_CODEC_NONE	0
#define PROLLY_CODEC_LZ4	1
#define PROLLY_FRAME_HEADER	5
extern void prolly_chunk_frame(const void *data, Size len, StringInfo out);
extern void prolly_chunk_frame_plain(const char *frame, Size len,
									 StringInfo out);
/*
 * What a remote's dolt_serve_format() says after the canonical format: it
 * sends and takes chunks framed, and (with the second) can decompress LZ4.
 */
#define PROLLY_SERVE_FRAMED	"/framed"
#define PROLLY_SERVE_LZ4	"+lz4"

/* (a session's: frames this server sends it, by get_stored, are plain) */
extern PGDLLIMPORT bool prolly_serve_uncompressed;

/* prolly_chunk_cache_size: kB of decoded chunks a backend keeps */
extern PGDLLIMPORT int prolly_chunk_cache_size;
extern bool prolly_chunk_unframe(const char *frame, Size len, StringInfo out);
extern bool prolly_chunk_frame_len(const char *frame, Size len, uint32 *chunk_len);

/*
 * A chunk of store as stored (its get_stored(), or its get() framed); a
 * table file added of chunks next() gives framed (its
 * add_stored_table_file(), or add_table_file() of them decoded).
 */
extern bool prolly_chunk_get_stored(const ProllyChunkStoreRoutine *cs,
									ProllyChunkStore *store,
									const ProllyHash *hash, StringInfo out);
extern void prolly_chunk_add_stored(const ProllyChunkStoreRoutine *cs,
									ProllyChunkStore *store,
									ProllyChunkNext next, void *arg);

/* The chunks a chunk names, as the server's trees lay their nodes out. */
extern void prolly_chunk_references(const void *data, Size len,
									ProllyChunkRefVisit visit, void *arg);

/*
 * What a store's chunks are: the content hash that names them (by name, and
 * its digest's length) and the version of the format the server's trees lay
 * their nodes out in.  A store records the format it is made in, and a
 * server of another refuses it.
 */
#define PROLLY_STORE_HASH_NAME_LEN	16

typedef struct ProllyStoreFormat
{
	char		hash[PROLLY_STORE_HASH_NAME_LEN];	/* zero-padded */
	uint8		hash_len;
	uint16		chunk_format;
} ProllyStoreFormat;

/* The server's (computed once, so first called outside a critical section). */
extern const ProllyStoreFormat *prolly_store_format(void);
extern bool prolly_store_format_equal(const ProllyStoreFormat *a,
									  const ProllyStoreFormat *b);
extern char *prolly_store_format_text(const ProllyStoreFormat *format);

#endif							/* PROLLY_CHUNKSTORE_H */
