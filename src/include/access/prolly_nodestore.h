/*-------------------------------------------------------------------------
 *
 * prolly_nodestore.h
 *	  Node store interface: finding, reading and writing nodes.
 *
 * Hash-addressed stores (D1, D2) find nodes by content hash through a
 * hash -> block map; location-addressed stores (D3, D4) by extent.  Nodes
 * are buffer-manager pages reached through smgr.
 *
 * Contract:
 * - read() and new_node() return a pinned buffer; the caller locks and
 *   releases it as for any Postgres buffer.  new_node() returns it
 *   exclusive-locked, with ref->location filled in.
 * - A node made findable by publish() stays readable until release().
 * - contains() answers from the store's own structures, without reading the
 *   node, and if location is not NULL sets it to where the node is.
 * - WAL logging of node contents is the log interface's job.  The store
 *   logs changes to its own structures (such as a hash -> block map) as
 *   ProllyStore records (prolly_xlog.h) and replays them in redo().
 *
 * Provisional (v0).  Remote and object stores (D5) may not fit the Buffer
 * return type; revisit if D5 is built.
 *
 * src/include/access/prolly_nodestore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_NODESTORE_H
#define PROLLY_NODESTORE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/xlogreader.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* Called with each node scan() finds. */
typedef void (*ProllyStoreVisit) (const ProllyHash *hash, BlockNumber block,
								  void *arg);

typedef struct ProllyNodeStoreRoutine
{
	ProllyImplInfo info;

	/* Create the store's storage (forks, maps) for a new tree. */
	void		(*create) (Relation rel, const RelFileLocator *rlocator,
						   char persistence);

	ProllyNodeStore *(*open) (Relation rel);
	void		(*close) (ProllyNodeStore *store);

	Buffer		(*read) (ProllyNodeStore *store, const ProllyNodeRef *ref);
	Buffer		(*new_node) (ProllyNodeStore *store, ProllyNodeRef *ref);

	/* Make the node in buf findable by hash; updates ref->hash. */
	void		(*publish) (ProllyNodeStore *store, Buffer buf,
							const ProllyHash *hash, ProllyNodeRef *ref);

	bool		(*contains) (ProllyNodeStore *store, const ProllyHash *hash,
							 ProllyExtent *location);

	/* Free the node's space; called by the reclamation interface. */
	void		(*release) (ProllyNodeStore *store, const ProllyNodeRef *ref);

	/*
	 * Reclamation.  scan() visits every node the store can find by hash
	 * (hash and block); reclaim() drops the one at block and gives its
	 * block to later new_node() calls once every transaction running now
	 * has ended (a reader may still hold a reference to it).  The caller
	 * holds the store exclusively (LockProllyStore), so nothing is written
	 * meanwhile; writers take it in RowExclusiveLock in new_node() and
	 * contains(), until their transaction ends.
	 */
	void		(*scan) (ProllyNodeStore *store, ProllyStoreVisit visit,
						 void *arg);
	void		(*reclaim) (ProllyNodeStore *store, const ProllyHash *hash,
							BlockNumber block);

	/* Replay a ProllyStore record this implementation wrote. */
	void		(*redo) (XLogReaderState *record);
} ProllyNodeStoreRoutine;

#define PROLLY_NODESTORE_CALLBACK_NAMES \
	"create", "open", "close", "read", "new_node", "publish", "contains", \
	"release", "scan", "reclaim", "redo"
#define PROLLY_NODESTORE_FIRST_CALLBACK create
#define PROLLY_NODESTORE_LAST_CALLBACK redo

static inline const ProllyNodeStoreRoutine *
prolly_nodestore_lookup(const char *name)
{
	return (const ProllyNodeStoreRoutine *)
		prolly_get_impl(PROLLY_IFACE_NODE_STORE, name);
}

#endif							/* PROLLY_NODESTORE_H */
