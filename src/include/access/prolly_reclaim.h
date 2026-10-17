/*-------------------------------------------------------------------------
 *
 * prolly_reclaim.h
 *	  Reclamation interface: deciding what is garbage and freeing it.
 *
 * Implementations: root-reachability GC (D1, D2), birth epoch (D3a), lazy
 * reference counts (D3b), in-page vacuum (D4).
 *
 * Contract:
 * - Nothing reachable from a root that a branch, a snapshot or a prepared
 *   transaction can still use is ever freed.
 * - may_modify_in_place() is true only if no root other than the caller's
 *   private one reaches node, so changing it in place is invisible to others.
 * - node_created/node_dropped/root_retired are notifications; an
 *   implementation that doesn't need one leaves it as a no-op, not NULL.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_reclaim.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_RECLAIM_H
#define PROLLY_RECLAIM_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "storage/buf.h"
#include "utils/relcache.h"

struct VacuumParams;

/* What one reclamation of the node store did. */
typedef struct ProllyGcResult
{
	uint64		nodes_kept;
	uint64		nodes_freed;
} ProllyGcResult;

typedef struct ProllyReclaimRoutine
{
	ProllyImplInfo info;

	bool		(*may_modify_in_place) (Relation rel, const ProllyNodeRef *node);

	void		(*node_created) (Relation rel, const ProllyNodeRef *node);
	void		(*node_dropped) (Relation rel, const ProllyNodeRef *node);
	void		(*root_retired) (ProllyBranchId branch, const ProllyNodeRef *root);

	/* One reclamation pass over rel (VACUUM or background worker). */
	void		(*run) (Relation rel, const struct VacuumParams *params,
						BufferAccessStrategy bstrategy);

	/*
	 * Reclaim every node of the cluster's node store that no live root
	 * reaches (prolly_gc()).  Waits for, and holds off, every transaction
	 * that writes nodes, until the current transaction ends.
	 */
	void		(*collect) (ProllyGcResult *result);
} ProllyReclaimRoutine;

#define PROLLY_RECLAIM_CALLBACK_NAMES \
	"may_modify_in_place", "node_created", "node_dropped", "root_retired", \
	"run", "collect"
#define PROLLY_RECLAIM_FIRST_CALLBACK may_modify_in_place
#define PROLLY_RECLAIM_LAST_CALLBACK collect

static inline const ProllyReclaimRoutine *
prolly_reclaim_lookup(const char *name)
{
	return (const ProllyReclaimRoutine *)
		prolly_get_impl(PROLLY_IFACE_RECLAMATION, name);
}

#endif							/* PROLLY_RECLAIM_H */
