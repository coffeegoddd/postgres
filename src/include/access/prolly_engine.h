/*-------------------------------------------------------------------------
 *
 * prolly_engine.h
 *	  Engine interface: the tree as a whole.
 *
 * An engine stores the trees of prolly tables and indexes and answers
 * reads, private writes, commit, checkpoint and canonical export.  Engines
 * D1-D4 are implementations.  The table AM and index AM adapters translate
 * Postgres calls into calls on this interface.
 *
 * An engine builds on the foundation interfaces (node format, chunker, hash,
 * encoders, node store, allocator, reclamation, log, commit pipeline,
 * coordination), which it gets through prolly_config.h, never by naming
 * another implementation.
 *
 * General contract:
 * - Operations act for the current transaction (GetCurrentTransactionId()
 *   and friends) unless a parameter says otherwise, as heap does.
 * - "tree" is the table or index relation whose tree is meant, and "kind"
 *   which of its trees: its data, or local metadata such as its row
 *   identity map (prolly_types.h).  A table and its indexes always come
 *   from the same root set.  A relation's trees are found by its storage
 *   (rd_locator.relNumber), so new storage (TRUNCATE, rewrites) starts a
 *   new tree and the old one stays readable until the transaction ends.
 * - Keys of a table's data tree are ordered by the table keying
 *   (prolly_keying.h); keys of local metadata trees by memcmp.
 * - A command sees the writes of earlier commands of its transaction, not
 *   its own (cid), as heap does.  A write tagged InvalidCommandId (rows
 *   rebuilt in another encoding, which are not new writes) is seen by
 *   every command, its own included.
 * - Returned keys and values stay valid until the next call on the same
 *   cursor, or until the cursor is closed.
 * - Errors are raised with ereport(); callbacks never return error codes.
 *
 * Provisional (v0).  Open questions that may reshape this interface: root
 * sets spanning tables of different engines; where redo of logical records
 * rebuilds nodes.
 *
 * src/include/access/prolly_engine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_H
#define PROLLY_ENGINE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/sdir.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

struct VacuumParams;

typedef struct ProllyEngineRoutine
{
	ProllyImplInfo info;

	/* Shared memory needed, and its initialization. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	/*
	 * Create an empty tree for rel in new storage at newrlocator (CREATE
	 * TABLE, TRUNCATE, rewrite).  Records the component format stamps.
	 */
	void		(*tree_create) (Relation rel, const RelFileLocator *newrlocator,
								char persistence);

	/* Make rel's tree empty in place, non-transactionally. */
	void		(*tree_truncate) (Relation rel);

	/*
	 * Empty one of rel's trees as a private write of the current
	 * transaction (to rebuild it).
	 */
	void		(*tree_reset) (Relation rel, ProllyTreeKind kind);

	/*
	 * Copy rel's trees, with the current transaction's writes, into new
	 * storage at newrlocator (SET TABLESPACE, CLUSTER, VACUUM FULL).
	 */
	void		(*tree_copy) (Relation rel, const RelFileLocator *newrlocator);

	/*
	 * The root set visible to snapshot: exactly one, consistent with
	 * procarray visibility for every xid.  Pinned until released.
	 */
	ProllyRootSet *(*roots_acquire) (Snapshot snapshot);
	void		(*roots_release) (ProllyRootSet *roots);

	/*
	 * Cursors over tree in roots, overlaid with the current transaction's own
	 * writes made before command cid (InvalidCommandId: all of them).
	 * cursor_seek positions before the first key >= key (forward) or after
	 * the last key <= key (backward); a NULL key means the start or end.
	 * cursor_next returns false at the end.
	 */
	ProllyCursor *(*cursor_open) (ProllyRootSet *roots, Relation tree,
								  ProllyTreeKind kind, CommandId cid);
	void		(*cursor_seek) (ProllyCursor *cursor, const ProllyKey *key,
								ScanDirection direction);
	bool		(*cursor_next) (ProllyCursor *cursor, ProllyKey *key,
								ProllyValue *value);
	void		(*cursor_close) (ProllyCursor *cursor);

	/* Look up key as a cursor would see it; on success append its value. */
	bool		(*get) (ProllyRootSet *roots, Relation tree, ProllyTreeKind kind,
						const ProllyKey *key, CommandId cid, StringInfo value);

	/*
	 * Number of keys in [lo, hi] (NULL = unbounded), from subtree counts;
	 * an estimate while the transaction has private writes to tree.
	 */
	/*
	 * Whether the current transaction has written tree's tree of kind (or
	 * given it new storage), so that its reads overlay private writes.
	 */
	bool		(*has_writes) (Relation tree, ProllyTreeKind kind);

	/*
	 * Append the key of the item at position ordinal (from 0) of tree's
	 * data tree in roots to key; false past the end.  Positions are only
	 * those of a tree the transaction hasn't written (has_writes false).
	 */
	bool		(*key_at) (ProllyRootSet *roots, Relation tree, uint64 ordinal,
						   StringInfo key);

	uint64		(*count_range) (ProllyRootSet *roots, Relation tree,
								ProllyTreeKind kind, const ProllyKey *lo,
								const ProllyKey *hi);

	/*
	 * Private writes of the current transaction, tagged with cid.  Locking
	 * is the coordination module's job and has happened before these calls.
	 */
	void		(*put) (Relation tree, ProllyTreeKind kind, const ProllyKey *key,
						const ProllyValue *value, CommandId cid);
	void		(*remove) (Relation tree, ProllyTreeKind kind,
						   const ProllyKey *key, CommandId cid);

	/* Discard writes of subxid and its children, or of the transaction. */
	void		(*subxact_abort) (SubTransactionId subxid);
	void		(*xact_abort) (void);

	/*
	 * Before commit: rebase the private writes onto the current branch
	 * heads, write them durably through the log, and hand the new roots to
	 * the commit pipeline.  Must not fail on conflicts: anything that could
	 * conflict was locked earlier, so the rebase is mechanical.
	 */
	void		(*xact_precommit) (void);

	/* After the commit record: publish what xact_precommit enqueued. */
	void		(*xact_commit) (void);

	/* PREPARE TRANSACTION: persist private writes for later commit. */
	void		(*xact_prepare) (void);

	/* Checkpoint: make everything up to the redo point durable. */
	void		(*checkpoint) (int flags);

	/* Make root the head of branch (checkout, reset, fast-forward). */
	void		(*install_root) (ProllyBranchId branch, const ProllyNodeRef *root);

	/* VACUUM: reclaim space through the reclamation interface. */
	void		(*vacuum) (Relation rel, const struct VacuumParams *params,
						   BufferAccessStrategy bstrategy);

	/*
	 * Canonical form: the root hash of tree in roots, and every canonical
	 * chunk reachable from it.  Identical across all engines for the same
	 * logical data.
	 */
	void		(*canonical_root) (ProllyRootSet *roots, Relation tree,
								   ProllyHash *root);
	void		(*export_chunks) (ProllyRootSet *roots, Relation tree,
								  ProllyChunkSink *sink);
} ProllyEngineRoutine;

#define PROLLY_ENGINE_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "tree_create", "tree_truncate", "tree_reset", \
	"tree_copy", \
	"roots_acquire", "roots_release", "cursor_open", "cursor_seek", \
	"cursor_next", "cursor_close", "get", "has_writes", "key_at", "count_range", \
	"put", "remove", \
	"subxact_abort", "xact_abort", "xact_precommit", "xact_commit", \
	"xact_prepare", "checkpoint", "install_root", "vacuum", "canonical_root", \
	"export_chunks"
#define PROLLY_ENGINE_FIRST_CALLBACK shmem_size

static inline const ProllyEngineRoutine *
prolly_engine_lookup(const char *name)
{
	return (const ProllyEngineRoutine *)
		prolly_get_impl(PROLLY_IFACE_ENGINE, name);
}

#endif							/* PROLLY_ENGINE_H */
