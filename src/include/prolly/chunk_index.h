/*-------------------------------------------------------------------------
 *
 * chunk_index.h
 *	  The history store's index: from a chunk's hash to where its bytes
 *	  are in the chunk log (prolly/chunk_log.h), and its reference counts.
 *
 * A ChunkIndexRoutine keeps one entry per chunk: its hash, the place and
 * length of its bytes in the log, the strong references to it from other
 * chunks (refs) and the references from outside the store (roots).
 * An entry is the chunk's once the transaction inserting it commits: until
 * then only that transaction finds it, and if it aborts (or was running at
 * a crash) nobody does, and inserts and VACUUM take the entry out.  An
 * insert of a chunk another transaction is inserting waits for it to end,
 * as an insert into a unique index does.  An entry's counts are changed in
 * place, WAL-logged, not transactionally.  A change gives its transaction
 * an id, so that its commit flushes it.  What is learned of the inserting
 * transaction is kept in the entry's flags, as heap tuples keep hint bits
 * (not WAL-logged); VACUUM freezes every committed entry, WAL-logged, so
 * that no entry needs the commit log once the index's relfrozenxid passes
 * it.
 *
 * PageChunkIndex keeps the entries in a relation of the prolly_chunk_index
 * table access method, by linear hashing:
 *
 *	block 0		the meta page: the buckets there are and the masks that
 *				map a hash to one, and the directory's pages
 *	directory	pages of the first page of each bucket, in bucket order
 *	buckets		a chain of pages per bucket, of entries in no order
 *
 * A lookup reads the directory page and the bucket's pages (one, mostly).
 * An insert that puts an entry past a bucket's first page splits a bucket,
 * the next in order, in two: the index grows a bucket at a time, and a
 * bucket stays a page long, mostly.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_index.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_INDEX_H
#define PROLLY_CHUNK_INDEX_H

#include "storage/smgr.h"
#include "utils/relcache.h"

/* A chunk's hash: the first bytes of the SHA-256 of its bytes. */
#define CHUNK_HASH_LEN	20

typedef struct ChunkIndexEntry
{
	uint8		hash[CHUNK_HASH_LEN];
	uint32		len;			/* the chunk's length */
	uint64		loc;			/* where its bytes begin in the log (a
								 * change's: its chunk's) */
	uint32		refs;			/* strong references from chunks */
	uint32		roots;			/* references from outside the store */
	TransactionId xmin;			/* the transaction that inserted it */
	TransactionId xmax;			/* a chunk's: the transaction that freed it,
								 * or 0; a change's: its chunk's xmin */
	uint16		flags;			/* CHUNK_ENTRY_*: what is known of xmin */
	uint16		kind;			/* CHUNK_KIND_* */
	uint32		log_gen;		/* a chunk's: the log's generation its bytes
								 * were reserved in (prolly/chunk_log.h) */
} ChunkIndexEntry;

/*
 * Sets a chunk's entry's place, loc and log_gen, as an insert is about to
 * put it: after everything that may wait or fail, so that it must not
 * fail itself (prolly/chunk_log.h's reserve).
 */
typedef void (*ChunkReserveFn) (void *arg, ChunkIndexEntry *entry);

/*
 * An entry is a chunk's, or a change of a chunk's counts: refs and roots,
 * as signed numbers, to add to the chunk's once the change's transaction
 * commits.  The counts of a chunk are its entry's and its committed
 * changes' (and, to the transaction making them, its own); VACUUM adds
 * committed changes into the chunk's entry, and takes them out.  A change
 * names the entry it is of by the entry's xmin and loc, so that a chunk
 * freed and put again starts with no changes of the one before.
 *
 * A chunk is freed by a transaction setting its entry's xmax: from then
 * the chunk is nobody's to put or find but by waiting for that transaction
 * (it is the chunk's again if it aborts), and once it commits the entry is
 * dead, for VACUUM to take out.
 */
#define CHUNK_KIND_CHUNK		0
#define CHUNK_KIND_CHANGE		1

#define CHUNK_ENTRY_COMMITTED	0x01	/* xmin committed (a hint) */
#define CHUNK_ENTRY_ABORTED		0x02	/* xmin aborted, or crashed (a hint) */
#define CHUNK_ENTRY_FROZEN		0x04	/* xmin committed, WAL-logged by VACUUM */
#define CHUNK_ENTRY_XMAX_COMMITTED	0x08	/* xmax committed (a hint) */
#define CHUNK_ENTRY_XMAX_ABORTED	0x10	/* xmax aborted, or crashed (a hint) */

typedef struct ChunkIndexRoutine
{
	/*
	 * The entry for hash into *out, its counts as this transaction sees
	 * them; false if there is none.
	 */
	bool		(*lookup) (Relation idx, const uint8 *hash,
						   ChunkIndexEntry *out);

	/*
	 * Inserts entry, its counts as given, unless one for its hash is there:
	 * then answers false, and that one in *entry.  reserve (if given) sets
	 * the entry's place once it is to be put, everything that may wait or
	 * fail before it done, and is called for no other entry: the bytes it
	 * names are then this entry's, whatever follows.
	 */
	bool		(*insert) (Relation idx, ChunkIndexEntry *entry,
						   ChunkReserveFn reserve, void *arg);

	/*
	 * Adds drefs and droots to hash's counts, in this transaction (they are
	 * the chunk's when it commits), and answers the entry's counts after,
	 * as this transaction sees them; false if there is no entry.  Taking a
	 * count below zero is an error; a decrease waits for another
	 * transaction decreasing the chunk's counts to end.  A transaction's
	 * changes of one chunk's counts are one change.
	 */
	bool		(*adjust) (Relation idx, const uint8 *hash, int32 drefs,
						   int32 droots, ChunkIndexEntry *out);

	/*
	 * Calls fn with each chunk's entry this transaction sees, its counts as
	 * it sees them.  For audits; O(index).
	 */
	void		(*visit) (Relation idx,
						  void (*fn) (void *arg, const ChunkIndexEntry *e),
						  void *arg);

	/*
	 * Marks the chunk's entry freed by this transaction, its entry (counts
	 * and all) in *out, if nothing holds it: its counts zero, no other
	 * transaction changing them, and held(hash) false (asked with the
	 * chunk's bucket locked, so that whatever looks the chunk up after
	 * sees it being freed).  False otherwise.
	 */
	bool		(*free) (Relation idx, const uint8 *hash,
						 bool (*held) (const uint8 *hash),
						 ChunkIndexEntry *out);

	/*
	 * The chunk log the index's entries are of (InvalidOid until it is
	 * bound to one) and its storage's stamp, and binding it to one: an
	 * index is one log storage's, which VACUUM counts the bytes of chunks
	 * it takes out dead in.
	 */
	Oid			(*log_of) (Relation idx, uint64 *stamp);
	void		(*bind_log) (Relation idx, Oid log, uint64 stamp);

	/* Removes hash's entry; false if there was none. */
	bool		(*remove) (Relation idx, const uint8 *hash);
} ChunkIndexRoutine;

extern PGDLLIMPORT const ChunkIndexRoutine *ChunkIndex;
extern PGDLLIMPORT const ChunkIndexRoutine PageChunkIndex;

/* The prolly_chunk_index table access method's relations are its. */
extern bool RelationIsChunkIndex(Relation rel);

/* For the access method: a new index's pages, written to its storage. */
extern void ChunkIndexInitStorage(SMgrRelation srel, bool use_wal);

/*
 * For the access method: takes out the entries of transactions that
 * aborted, notes on every other whether its transaction committed, and
 * advances the relation's relfrozenxid.
 */
extern void ChunkIndexVacuum(Relation idx);

/* For tests: the entries on the buckets' pages not frozen. */
extern uint64 ChunkIndexUnfrozen(Relation idx);

/*
 * For tests: the buckets there are, the index's pages, and the entries on
 * the buckets' pages (copies a split left included).
 */
extern void ChunkIndexShape(Relation idx, uint32 *buckets, uint32 *pages,
							uint64 *entries);

#endif							/* PROLLY_CHUNK_INDEX_H */
