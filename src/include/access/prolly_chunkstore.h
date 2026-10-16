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

	/*
	 * Give a new database (CREATE DATABASE's, its files copied or not) the
	 * store of the one it was made from, WAL-logged as any change.
	 */
	void		(*copy_database) (Oid src_dboid, Oid src_spcoid,
								  Oid dst_dboid, Oid dst_spcoid);
} ProllyChunkStoreRoutine;

#define PROLLY_CHUNKSTORE_CALLBACK_NAMES \
	"open", "put", "get", "has", "root", "move_root", "commit_payload", \
	"committed", "commit_redo", "checkpoint", "redo", "copy_database"
#define PROLLY_CHUNKSTORE_FIRST_CALLBACK open

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
