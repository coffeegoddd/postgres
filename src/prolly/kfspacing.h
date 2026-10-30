/*
 * kfspacing.h
 *	  Keyframe spacing: at which commits keyframes are taken, and which held
 *	  keyframes are let go of as the head moves on.
 *
 * Positions are a commit's change bytes (VcCommit.bytes): its own change
 * and the largest parent's, so the change between a commit and an
 * ancestor is the difference of theirs, known with no diff.  Distances are
 * measured back from the head.
 *
 * Two policies:
 *	even		a keyframe every 'delta' bytes of change, kept while the window
 *				keeps its commit: a move to any commit in the window replays
 *				at most about delta / 2, and keyframes held grow with history.
 *	exponential	dense near the head and sparser with age: a keyframe every
 *				'delta0' bytes at the head, one let go of once the gap it
 *				would leave is within spacing(d) = max(delta0, d (r - 1) / r),
 *				d its distance.  Every gap is then at most about spacing(d),
 *				so a commit d back is within about d (r - 1) / (2 r) of a
 *				keyframe, and keyframes held grow with the logarithm of
 *				history, so no window is needed to bound them.
 */
#ifndef KFSPACING_H
#define KFSPACING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct KfSpacing KfSpacing;

typedef struct KfSpacingOps
{
	/*
	 * Whether to take a keyframe at the head, at position 'head', the newest
	 * keyframe held being at 'newest' (0 with none).
	 */
	bool		(*take) (KfSpacing *s, uint64_t head, uint64_t newest);

	/*
	 * Which of the 'n' held keyframes, at positions 'at' (oldest first), to
	 * let go of now that the head is at 'head': drop[i] set for each.
	 * Returns how many.  The newest is always kept.
	 */
	size_t		(*thin) (KfSpacing *s, uint64_t head, const uint64_t *at, size_t n,
						 bool *drop);

	void		(*destroy) (KfSpacing *s);
} KfSpacingOps;

struct KfSpacing
{
	const KfSpacingOps *ops;
};

extern KfSpacing *kf_even_spacing(uint64_t delta);
extern KfSpacing *kf_exponential_spacing(uint64_t delta0, double r);

/* The spacing an exponential policy keeps at distance 'd' from the head. */
extern uint64_t kf_exponential_gap(uint64_t delta0, double r, uint64_t d);

#endif							/* KFSPACING_H */
