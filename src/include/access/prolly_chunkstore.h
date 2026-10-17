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
 *   keep one copy or several).  Every change to the store's files is
 *   WAL-logged, so recovery, standbys and base backups carry the history,
 *   and a database made from another (CREATE DATABASE) gets a copy of it.
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
 *   is.  conjoin() rewrites the table files as one.  table_files() visits
 *   the store's table files.
 * - shmem_size() and shmem_init() reserve and set up the shared state of
 *   the server's stores (optional).
 * - read_through() makes this backend's get() and has() ask source for a
 *   chunk the store hasn't, until it is cleared (source NULL) or the
 *   transaction ends: so walks that know what chunks hold can read another
 *   store's (a transfer's) as if it were this one's.
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

/* The next chunk to add, in hash and data; false when there are no more. */
typedef bool (*ProllyChunkNext) (void *arg, ProllyHash *hash, StringInfo data);

/*
 * Reads a chunk from elsewhere into out (NULL: only whether it has it);
 * false if it has none by that hash.
 */
typedef bool (*ProllyChunkGet) (void *arg, const ProllyHash *hash, StringInfo out);

/* Visits a table file: its name, its chunk count and its size in bytes. */
typedef void (*ProllyChunkFileVisit) (const char *name, int nchunks,
									  uint64 size, void *arg);

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
	 * Commit.  commit_payload() returns what the commit record carries for
	 * the transaction's root move (0 bytes: none); committed() is called
	 * once the commit record is flushed, inside the commit's critical
	 * section; commit_redo() replays the payload.
	 */
	Size		(*commit_payload) (char **data);
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
								 void *arg);
} ProllyChunkStoreRoutine;

#define PROLLY_CHUNKSTORE_CALLBACK_NAMES \
	"open", "put", "get", "has", "root", "move_root", "commit_payload", \
	"committed", "commit_redo", "checkpoint", "redo", "add_table_file", \
	"conjoin", "table_files", "shmem_size", "shmem_init", "copy_database", \
	"read_through"
#define PROLLY_CHUNKSTORE_FIRST_CALLBACK open
#define PROLLY_CHUNKSTORE_LAST_CALLBACK read_through

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
extern void prolly_chunkstore_committed(void);
extern void prolly_chunkstore_commit_redo(const char *data, Size len);
extern void prolly_chunkstore_checkpoint(void);

#endif							/* PROLLY_CHUNKSTORE_H */
