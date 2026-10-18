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
 *   that hash.  has() answers without reading the chunk.
 * - root() is the root as last committed (or as moved by the calling
 *   transaction); an empty hash if it never was.
 * - move_root() moves the root with the calling transaction, if it is still
 *   expected (what the caller built on): false, without moving, if another
 *   transaction moved it since.  It waits for other movers, and they for
 *   it, until its transaction ends; the move is made durable by the
 *   transaction's commit record, and is gone if the transaction aborts.
 * - A transaction can't be prepared (two-phase) after moving a root.
 * - add_table_file() adds a table file of the chunks next() gives (until it
 *   returns false), at once and for every transaction: the chunks of
 *   another store, received whole, or of a rewrite.  It isn't undone if the
 *   transaction aborts; its chunks are reachable once a root naming them
 *   is.  conjoin() rewrites the table files as one (settled if they all
 *   were).  table_files() visits the store's table files: a settled one is
 *   closed under what its chunks name (they are in it, in another settled
 *   file, or in the journal's segments).
 * - The journal (where put() appends) is in segments: seal() closes the one
 *   appends go to, and they go on to a new one; segments() visits them,
 *   oldest first (with count, each with its chunks counted; without, only
 *   whether it has any, nchunks 0 or -1, so nothing is read but sizes).
 * - Garbage collection is the store's side of a collector's work
 *   (prolly_reclaim.h), in this order, in one transaction:
 *   gc_begin() holds off conjoins and new table files until the
 *   transaction ends (another collection meanwhile is refused, as is one
 *   in a transaction that used the store), seals the journal, and numbers
 *   the chunks collected (slots 0 .. *nslots - 1): the sealed segments',
 *   and the table files' (with full all of them, else those not settled),
 *   each file's slots together, as gc_files() visits them; *root is the
 *   root as of the seal.  gc_find() gives a chunk's slot (-1: not collected), gc_read() a
 *   slot's bytes.  gc_new() visits what is new since the seal, or since its
 *   last call: the root, and each chunk appended since; the first call waits
 *   for the transactions that appended to the sealed segments and were in
 *   progress at the seal, and with window true it holds every append off
 *   until gc_swap().  gc_copy() writes the marked slots (but those of files
 *   kept whole) to new, settled table files, before the window; gc_swap()
 *   writes those marked since, and replaces the manifest: the new files and
 *   those kept, the segment appends go to, the next GC generation.  What
 *   was collected and not kept is no longer found; gc_retire() removes it
 *   once every transaction running at the swap has ended.  A transaction
 *   running at a swap that then appends a chunk naming one no longer found
 *   fails (a serialization failure), as does one adding a table file
 *   (add_table_file()) whose transaction began before the swap.
 * - shmem_size() and shmem_init() reserve and set up the shared state of
 *   the server's stores (optional).
 * - read_through() makes this backend's get() and has() ask source for a
 *   chunk the store hasn't, until it is cleared (source NULL) or the
 *   transaction ends: so walks that know what chunks hold can read another
 *   store's (a transfer's) as if it were this one's.  Its hint, if given
 *   (with or without a source), hears what prefetch() is told.
 * - prefetch() is told the chunks a walk is about to read (a node's
 *   children), so that a read-through source can fetch them at once
 *   (optional): whether a hint heard them, and with none (n 0), whether
 *   one would, so a walk collects them only when it would.
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
#include "lib/stringinfo.h"

typedef struct ProllyChunkStore ProllyChunkStore;

/* Visits a chunk another chunk names. */
typedef void (*ProllyChunkRefVisit) (const ProllyHash *ref, void *arg);

/* The next chunk to add, in hash and data; false when there are no more. */
typedef bool (*ProllyChunkNext) (void *arg, ProllyHash *hash, StringInfo data);

/*
 * Reads a chunk from elsewhere into out (NULL: only whether it has it);
 * false if it has none by that hash.
 */
typedef bool (*ProllyChunkGet) (void *arg, const ProllyHash *hash, StringInfo out);

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

	/* Replay the store's own records. */
	void		(*redo) (XLogReaderState *record);

	/* Table files. */
	void		(*add_table_file) (ProllyChunkStore *store, ProllyChunkNext next,
								   void *arg);
	void		(*conjoin) (ProllyChunkStore *store);
	void		(*table_files) (ProllyChunkStore *store,
								ProllyChunkFileVisit visit, void *arg);

	/* Shared state. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	/*
	 * Give a new database (CREATE DATABASE's, its files copied or not) the
	 * store of the one it was made from, WAL-logged as any change.
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
	ProllyChunkGc *(*gc_begin) (ProllyChunkStore *store, bool full,
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
} ProllyChunkStoreRoutine;

#define PROLLY_CHUNKSTORE_CALLBACK_NAMES \
	"open", "put", "get", "has", "root", "move_root", "commit_payload", \
	"commit_writes", "committed", "commit_redo", "checkpoint", "redo", \
	"add_table_file", \
	"conjoin", "table_files", "shmem_size", "shmem_init", "copy_database", \
	"read_through", "prefetch", "seal", "segments", "gc_begin", "gc_files", \
	"gc_find", "gc_read", "gc_new", "gc_copy", "gc_swap", "gc_retire"
#define PROLLY_CHUNKSTORE_FIRST_CALLBACK open
#define PROLLY_CHUNKSTORE_LAST_CALLBACK gc_retire

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

/* The chunks a chunk names, as the server's trees lay their nodes out. */
extern void prolly_chunk_references(const void *data, Size len,
									ProllyChunkRefVisit visit, void *arg);

#endif							/* PROLLY_CHUNKSTORE_H */
