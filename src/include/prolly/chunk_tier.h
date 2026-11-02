/*-------------------------------------------------------------------------
 *
 * chunk_tier.h
 *	  The history store's tier: a chunk log whose bytes are in pack objects
 *	  off the server (prolly/object_store.h), its index on the server.
 *
 * A tier log is a relation of the prolly_tier access method: its pages are
 * the catalog of its packs, a pack an immutable object of chunks' bytes one
 * after another.  The index's entries (prolly/chunk_index.h) are as the
 * pages store's, a chunk's place being its pack's number (the high 32
 * bits) and where in the pack its bytes begin (the low): so counts, holds
 * and frees are transactional, WAL-logged and on hot standbys as there.
 *
 * A transaction's puts gather their chunks into a pack of the backend's, a
 * number the catalog gives it (its entry the transaction's, WAL-logged),
 * written whole to the object store as the transaction commits, before the
 * commit record, and its size then recorded; a pack grown to
 * TIER_PACK_BYTES is written then, and another begun.  A pack whose
 * transaction aborted, or a crash cut short, is no committed entry's: VACUUM
 * removes its object, if one was written.  A pack all of whose bytes are
 * dead (its chunks' entries out) is removed too.
 *
 * The objects are where history_tier_store (an implementation, "directory")
 * at history_tier_location keeps them, the names "packs/" and the log's
 * stamp, the pack's owner (the server's that made it) and its number.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_tier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_TIER_H
#define PROLLY_CHUNK_TIER_H

#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/object_store.h"
#include "storage/smgr.h"

/* The bytes a pack has at most, but for one chunk longer, alone in its own. */
#define TIER_PACK_BYTES		((uint64) 64 * 1024 * 1024)

/*
 * A pack's object: its chunks' bytes one after another, then its table (an
 * entry a chunk, by hash, then by offset), then its footer, numbers in
 * network byte order.  So a pack says what it has without the server's
 * index.  Every chunk written in it has an entry, its bytes its hash's;
 * empty chunks, in no pack, have none.  An entry's chunk may be dead (its
 * index entry gone), and a hash may be in two (a chunk put, its savepoint
 * rolled back, and put again): the index says which is live.
 */
#define CHUNK_TIER_PACK_MAGIC	UINT64CONST(0x4c5052595041434b)	/* "LPRYPACK" */
#define CHUNK_TIER_PACK_VERSION	1

typedef struct ChunkTierPackEntry
{
	uint8		hash[CHUNK_HASH_LEN];
	uint32		off;
	uint32		len;
} ChunkTierPackEntry;

typedef struct ChunkTierPackFooter
{
	uint32		nentries;
	uint32		version;
	uint64		data_len;		/* the chunks' bytes, before the table */
	uint64		magic;
} ChunkTierPackFooter;

StaticAssertDecl(sizeof(ChunkTierPackEntry) == 28, "a pack's entry is 28 bytes");
StaticAssertDecl(sizeof(ChunkTierPackFooter) == 24, "a pack's footer is 24 bytes");

/*
 * The table of the pack name (palloc'd, in *entries, numbers in this
 * server's order), and the length of its chunks' bytes in *data_len;
 * answers its entries.  An error if it is not there, or not a pack of this
 * format, or its footer does not fit its size, or an entry is out of its
 * chunks' bytes or of order.
 */
extern int	ChunkTierReadPackTable(ObjectStore *os, const char *name,
								   ChunkTierPackEntry **entries, uint64 *data_len);

/* The table of the tier's pack id (written), as ChunkTierReadPackTable. */
extern int	ChunkTierPackTable(Relation rel, uint32 id, ChunkTierPackEntry **entries,
							   uint64 *data_len);

extern PGDLLIMPORT char *history_tier_store;
extern PGDLLIMPORT char *history_tier_location;

extern PGDLLIMPORT const ChunkLogRoutine TierChunkLog;

/* For the access method: a new tier log's first page, written to its storage. */
extern void ChunkTierInitStorage(SMgrRelation srel, bool use_wal);

/*
 * For the access method: a tier log's storage cut in place (TRUNCATE in the
 * transaction that made it), this backend's pack of it let go: its chunks
 * went with the index cut with it.
 */
extern void ChunkTierTruncated(Relation rel);

/*
 * For the access method: a tier log about to have new storage (TRUNCATE),
 * refused while this backend has a pack of it not yet written: the pack's
 * entry is the storage's now, and a subtransaction's rollback would bring
 * it back.
 */
extern void ChunkTierNewStorage(Relation rel);

/*
 * For DROP and TRUNCATE (what, the verb): refused while the tier has a
 * pack not removed (an object of it there, or that may be), which the
 * storage going would leave in the store, named by no catalog.  Release
 * its bundles, free its chunks and VACUUM its index, and it is empty.
 */
extern void ChunkTierCheckEmpty(Relation rel, const char *what);

/*
 * For DROP and TRUNCATE of a tier found empty: the database's marker of it
 * removed (one left by a pack's beginning cut short would else stay, and
 * keep the database from being dropped).  Its mark cleared first, so that
 * one rolled back is as one emptied.
 */
extern void ChunkTierForgetMarker(Relation rel);

/*
 * The same, for DROP and TRUNCATE of a chunk index bound to a tier (its
 * stamp the tier's now): refused while the tier has packs, which it alone
 * could free.
 */
extern void ChunkTierCheckIndex(Relation idx, const char *what);

/*
 * Whether the database dboid's tiers have packs (a marker of each that has,
 * in the store): for DROP DATABASE, and CREATE DATABASE from it as a
 * template, refused while they have.  The place set now is the one looked
 * in: with none set, or another than its packs were put at, nothing is
 * found (a tier's own puts and VACUUMs refuse another place; changing
 * history_tier_location while packs are there, and then dropping their
 * database, is not supported).
 */
extern bool ChunkTierDatabaseHasPacks(Oid dboid);

/*
 * A bundle's manifest (prolly/chunk_push.c's, its bytes the tier's to
 * keep, not to read), in the tier: an entry of the catalog of its own kind,
 * this transaction's, its object written now under manifests/, the entry's
 * record flushed first; answers its number.  Aborted, or cut short by a
 * crash, it is an orphan, removed by VACUUM.  Dropped, by a release: once
 * that commits, VACUUM counts it all dead and removes it.  Listed: each
 * manifest the tier's store has, its bytes passed to fn.
 */
extern uint32 ChunkTierWriteManifest(Relation rel, const void *data, size_t len);
extern void ChunkTierDropManifest(Relation rel, uint32 id);
extern void ChunkTierListManifests(Relation rel,
								   void (*fn) (void *arg, const char *data, size_t len),
								   void *arg);

/*
 * For the access method: VACUUM of a tier log, every pack whose transaction
 * ended settled (committed, or orphaned) and its relfrozenxid advanced; no
 * object is touched (VACUUM of its index removes them).
 */
extern void ChunkTierVacuum(Relation rel);

/* For tests: the tier's packs, a state each ("made", "committed", ...). */
typedef struct ChunkTierPackInfo
{
	uint32		id;
	const char *state;
	uint64		size;
	uint64		dead;
	uint64		owner;
	const char *kind;			/* "pack" or "manifest" */
} ChunkTierPackInfo;

extern int	ChunkTierPacks(Relation rel, ChunkTierPackInfo **out);

/*
 * Every pack the tier has made so far this server's to remove, whatever
 * its owner (those from before a promotion, or a recovery to a point in
 * time): for when no other server uses them.  Not transactional (WAL-logged
 * at once).  Answers how many packs' numbers it covers that it did not
 * before, removed ones and this server's own included.
 */
extern int	ChunkTierAdopt(Relation rel);

#endif							/* PROLLY_CHUNK_TIER_H */
