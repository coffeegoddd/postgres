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
 * transaction is kept in the entry's flags, as heap tuples keep hint bits;
 * VACUUM writes it to every entry (WAL-logged), so that an entry needs the
 * commit log only until the index's relfrozenxid passes it.
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
	uint64		loc;			/* where its bytes begin in the log */
	uint32		refs;			/* strong references from chunks */
	uint32		roots;			/* references from outside the store */
	TransactionId xmin;			/* the transaction that inserted it */
	uint16		flags;			/* CHUNK_ENTRY_*: what is known of xmin */
	uint16		unused;
} ChunkIndexEntry;

#define CHUNK_ENTRY_COMMITTED	0x01	/* xmin committed */
#define CHUNK_ENTRY_ABORTED		0x02	/* xmin aborted (or crashed) */

typedef struct ChunkIndexRoutine
{
	/* The entry for hash into *out; false if there is none. */
	bool		(*lookup) (Relation idx, const uint8 *hash,
						   ChunkIndexEntry *out);

	/*
	 * Inserts entry, its counts as given, unless one for its hash is there:
	 * then answers false, and that one in *entry.
	 */
	bool		(*insert) (Relation idx, ChunkIndexEntry *entry);

	/*
	 * Adds drefs and droots to hash's counts, and answers the entry after;
	 * false if there is none.  Taking a count below zero is an error.
	 */
	bool		(*adjust) (Relation idx, const uint8 *hash, int32 drefs,
						   int32 droots, ChunkIndexEntry *out);

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

/*
 * For tests: the buckets there are, the index's pages, and the entries on
 * the buckets' pages (copies a split left included).
 */
extern void ChunkIndexShape(Relation idx, uint32 *buckets, uint32 *pages,
							uint64 *entries);

#endif							/* PROLLY_CHUNK_INDEX_H */
