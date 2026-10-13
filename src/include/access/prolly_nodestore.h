/*-------------------------------------------------------------------------
 *
 * prolly_nodestore.h
 *	  Node store interface: finding, reading and writing nodes.
 *
 * Hash-addressed stores (D1, D2) find nodes by content hash through a
 * hash -> block map; location-addressed stores (D3, D4) by extent.  Nodes
 * are buffer-manager pages reached through smgr (B4, open question 1).
 *
 * Contract:
 * - read() and new_node() return a pinned buffer; the caller locks and
 *   releases it as for any Postgres buffer.  new_node() returns it
 *   exclusive-locked, with ref->location filled in.
 * - A node made findable by publish() stays readable until release().
 * - WAL logging is the log interface's job, not the store's.
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
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

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

	bool		(*contains) (ProllyNodeStore *store, const ProllyHash *hash);

	/* Free the node's space; called by the reclamation interface. */
	void		(*release) (ProllyNodeStore *store, const ProllyNodeRef *ref);
} ProllyNodeStoreRoutine;

#define PROLLY_NODESTORE_CALLBACK_NAMES \
	"create", "open", "close", "read", "new_node", "publish", "contains", \
	"release"
#define PROLLY_NODESTORE_FIRST_CALLBACK create

static inline const ProllyNodeStoreRoutine *
prolly_nodestore_lookup(const char *name)
{
	return (const ProllyNodeStoreRoutine *)
		prolly_get_impl(PROLLY_IFACE_NODE_STORE, name);
}

#endif							/* PROLLY_NODESTORE_H */
