/*
 * kfspacing.c
 *	  Keyframe spacing policies: even, and exponential.
 */
#include <stdlib.h>

#include "kfspacing.h"

/* ---------------------------------------------------------------- even */

typedef struct EvenSpacing
{
	KfSpacing	base;
	uint64_t	delta;
} EvenSpacing;

static bool
even_take(KfSpacing *s, uint64_t head, uint64_t newest)
{
	EvenSpacing *e = (EvenSpacing *) s;

	return newest == 0 || head >= newest + e->delta;
}

/* Even spacing lets go of nothing: the window releases what leaves it. */
static size_t
even_thin(KfSpacing *s, uint64_t head, const uint64_t *at, size_t n, bool *drop)
{
	for (size_t i = 0; i < n; i++)
		drop[i] = false;
	return 0;
}

static void
spacing_destroy(KfSpacing *s)
{
	free(s);
}

static const KfSpacingOps even_ops = {even_take, even_thin, spacing_destroy};

KfSpacing *
kf_even_spacing(uint64_t delta)
{
	EvenSpacing *e = calloc(1, sizeof(*e));

	e->base.ops = &even_ops;
	e->delta = delta ? delta : 1;
	return &e->base;
}

/* --------------------------------------------------------- exponential */

typedef struct ExpSpacing
{
	KfSpacing	base;
	uint64_t	delta0;
	double		r;
} ExpSpacing;

uint64_t
kf_exponential_gap(uint64_t delta0, double r, uint64_t d)
{
	double		g = (double) d * (r - 1) / r;

	return g > (double) delta0 ? (uint64_t) g : delta0;
}

static bool
exp_take(KfSpacing *s, uint64_t head, uint64_t newest)
{
	ExpSpacing *x = (ExpSpacing *) s;

	return newest == 0 || head >= newest + x->delta0;
}

/*
 * A keyframe is let go of when the gap it would leave, from the keyframe
 * kept newer than it to the one older than it, is no more than the
 * spacing its distance allows; the newest and the oldest are always kept.
 * So every gap is at most the spacing at its distance (plus the gap it
 * merged into, at most delta0 at the head), and keyframes thin out as the
 * head moves on, the gaps only growing: one let go of never comes back.
 */
static size_t
exp_thin(KfSpacing *s, uint64_t head, const uint64_t *at, size_t n, bool *drop)
{
	ExpSpacing *x = (ExpSpacing *) s;
	size_t		dropped = 0;
	uint64_t	kept;

	for (size_t i = 0; i < n; i++)
		drop[i] = false;
	if (n < 3)
		return 0;
	kept = at[n - 1];
	for (size_t i = n - 1; i-- > 1;)
	{
		uint64_t	d = head > at[i] ? head - at[i] : 0;

		if (kept - at[i - 1] <= kf_exponential_gap(x->delta0, x->r, d))
		{
			drop[i] = true;
			dropped++;
		}
		else
			kept = at[i];
	}
	return dropped;
}

static const KfSpacingOps exp_ops = {exp_take, exp_thin, spacing_destroy};

KfSpacing *
kf_exponential_spacing(uint64_t delta0, double r)
{
	ExpSpacing *x = calloc(1, sizeof(*x));

	x->base.ops = &exp_ops;
	x->delta0 = delta0 ? delta0 : 1;
	x->r = r > 1 ? r : 2;
	return &x->base;
}
