/*-------------------------------------------------------------------------
 *
 * prolly_reclaim.h
 *	  Reclamation interface: deciding what is garbage and freeing it.
 *
 * Implementations: root-reachability GC (D1, D2), birth epoch (D3a), lazy
 * reference counts (D3b), in-page vacuum (D4).  See
 * specs/03-design-space.md, axis 1.
 *
 * Contract:
 * - Nothing reachable from a root that a branch, a snapshot or a prepared
 *   transaction can still use is ever freed.
 * - may_modify_in_place() is true only if no root other than the caller's
 *   private one reaches node (the in-place constraint in axis 1).
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
} ProllyReclaimRoutine;

#define PROLLY_RECLAIM_CALLBACK_NAMES \
	"may_modify_in_place", "node_created", "node_dropped", "root_retired", \
	"run"
#define PROLLY_RECLAIM_FIRST_CALLBACK may_modify_in_place

static inline const ProllyReclaimRoutine *
prolly_reclaim_lookup(const char *name)
{
	return (const ProllyReclaimRoutine *)
		prolly_get_impl(PROLLY_IFACE_RECLAMATION, name);
}

#endif							/* PROLLY_RECLAIM_H */
