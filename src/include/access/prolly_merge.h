/*-------------------------------------------------------------------------
 *
 * prolly_merge.h
 *	  Diff and merge interface: comparing and combining versions.
 *
 * Diff and three-way merge work on canonical trees, so they don't depend on
 * the engine that stores either side.  Structural sharing lets a diff skip
 * every subtree whose hash is equal on both sides.
 *
 * Contract:
 * - diff() reports every row that differs between the two refs, once, in
 *   key order per relation.
 * - merge() merges branch into the current branch and returns the merge
 *   commit's id; conflicts raise an error and change nothing (conflict
 *   reporting is not designed yet).
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_merge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MERGE_H
#define PROLLY_MERGE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"

typedef struct ProllyMergeRoutine
{
	ProllyImplInfo info;

	void		(*diff) (const char *from_ref, const char *to_ref,
						 ProllyDiffSink *sink);
	void		(*merge) (const char *branch, ProllyHash *commit_id);
} ProllyMergeRoutine;

#define PROLLY_MERGE_CALLBACK_NAMES "diff", "merge"
#define PROLLY_MERGE_FIRST_CALLBACK diff

static inline const ProllyMergeRoutine *
prolly_merge_lookup(const char *name)
{
	return (const ProllyMergeRoutine *)
		prolly_get_impl(PROLLY_IFACE_DIFF_MERGE, name);
}

#endif							/* PROLLY_MERGE_H */
